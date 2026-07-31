//========================================================================//
//
// weapon_barnacle.cpp  (HL2MP / unified 2025 SDK layout version)
//
// Barnacle Gun prototype - grapple weapon inspired by the Barnacle
// Grapple from Half-Life: Opposing Force.
//
//   Primary fire   - fires a "tongue" (server-side swept trace that
//                    extends over time at a fixed speed).
//   On valid hit   - the tongue attaches to the surface / entity.
//   Secondary fire - while attached, HOLD to pull the player towards
//                    the attach point. Release: stop pulling, stay
//                    attached (momentum capped). Primary while
//                    attached: cancel.
//
// Explicit state machine:  IDLE -> FIRING -> ATTACHED <-> PULLING
//
// ARCHITECTURE NOTE (prototype scope):
// This is a shared class (required so the client can receive the entity),
// but ALL gameplay logic runs server-side only - there is no client
// prediction. On a listen server / effectively-singleplayer session this
// is indistinguishable from predicted movement, which matches the
// assignment ("SDK Base 2013 singleplayer" scope). For real networked
// multiplayer, the pulling math would move into CGameMovement and the
// attach state into the prediction tables - see docs/DESIGN.md §6.
//
// No custom assets: crossbow models via scripts/weapon_barnacle.txt,
// tongue visualized with a stock-sprite CBeam.
//
//========================================================================//

#include "cbase.h"
#include "in_buttons.h"

#ifdef CLIENT_DLL
	#include "c_hl2mp_player.h"
	#include "c_baseviewmodel.h"
	#include "beamdraw.h"
	#include "materialsystem/imaterialsystem.h"
	#include "materialsystem/imaterial.h"
	#include "materialsystem/imesh.h"
	#include "materialsystem/MaterialSystemUtil.h"
	#include "debugoverlay_shared.h"
#else
	#include "hl2mp_player.h"
	#include "beam_shared.h"
	#include "ndebugoverlay.h"
	#include "physics_shared.h"		// physprops
	#include "engine/IEngineTrace.h"
#endif

#include "weapon_hl2mpbasehlmpcombatweapon.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

//-----------------------------------------------------------------------------
// Shared tunables.
//
// These are needed by BOTH dlls: the server drives gameplay with them, the
// client draws the tongue with them. FCVAR_REPLICATED means one definition,
// one value, no chance of the two sides drifting apart.
//-----------------------------------------------------------------------------
static ConVar sk_barnacle_range        ( "sk_barnacle_range",         "1200", FCVAR_REPLICATED, "Max tongue length in units, measured along the wrapped path." );
static ConVar sk_barnacle_tongue_speed ( "sk_barnacle_tongue_speed",  "3500", FCVAR_REPLICATED, "Speed at which the tongue tip extends (units/sec)." );
static ConVar barnacle_wrap            ( "barnacle_wrap",             "1",    FCVAR_REPLICATED, "0 = straight tongue (original behaviour), 1 = wrap around corners." );

//-----------------------------------------------------------------------------
// Shared wrapping constants.
//-----------------------------------------------------------------------------
#define BARNACLE_MAX_PIVOTS       8		// hard cap on bends; beyond this we stop adding
#define BARNACLE_PIVOT_OFFSET     2.0f	// how far a pivot is pushed off the surface (units)
#define BARNACLE_REWRAP_GUARD     8.0f	// a bend released this frame will not be recreated within this radius
#define BARNACLE_CORNER_STEPS     8		// fallback corner search resolution
#define BARNACLE_WRAPS_PER_FRAME  3		// max new pivots per solver pass (runaway guard)
#define BARNACLE_SOLVER_PASSES    4		// unwrap/wrap passes run until stable, up to this
#define BARNACLE_CORNER_MAX_DIST  96.0f	// sanity: a solved corner further than this from the
										// blocking hit is rejected as degenerate

// Which geometry the tongue is allowed to wrap around. Brush-only on purpose:
// physics props have concave/complex collision that produces ugly, jittery
// pivots, and you do not want the tongue neatly wrapping a rolling barrel.
#define BARNACLE_WRAP_MASK        MASK_SOLID_BRUSHONLY

//-----------------------------------------------------------------------------
// One bend of the tongue. Stored in entity-local space as well as world space
// so a pivot that sits on a moving brush entity (door, elevator, train) tracks
// it - same trick already used for the attach point.
//-----------------------------------------------------------------------------
struct BarnaclePivot_t
{
	Vector	vecPos;		// world space, refreshed every frame
	Vector	vecLocal;	// position in hEnt's local space (unused if hEnt is NULL)
	Vector	vecNormal;	// surface normal we were pushed away from
	EHANDLE	hEnt;		// NULL = static world geometry
};

#ifndef CLIENT_DLL
//-----------------------------------------------------------------------------
// Server-only tunables. Live-tunable so movement quality can be iterated on
// without recompiling. Defaults chosen to feel "controlled" (DESIGN.md).
//-----------------------------------------------------------------------------
static ConVar sk_barnacle_pull_speed   ( "sk_barnacle_pull_speed",    "550",  FCVAR_NONE, "Target speed the player is pulled at (units/sec)." );
static ConVar sk_barnacle_corner_lead  ( "sk_barnacle_corner_lead",   "48",   FCVAR_NONE, "How far past a bend the pull aims, along the next segment. Steers the player around the corner instead of into it." );
static ConVar sk_barnacle_corner_clear ( "sk_barnacle_corner_clear",  "24",   FCVAR_NONE, "How far off the surface the pull aims at a bend. Must exceed the player's half-width or the target is unreachable." );
static ConVar sk_barnacle_pull_accel   ( "sk_barnacle_pull_accel",    "1600", FCVAR_NONE, "How fast player velocity converges to the pull vector (units/sec^2)." );
static ConVar sk_barnacle_detach_dist  ( "sk_barnacle_detach_dist",   "72",   FCVAR_NONE, "Distance from the anchor at which we auto-detach (arrival)." );
static ConVar sk_barnacle_release_speed( "sk_barnacle_release_speed", "400",  FCVAR_NONE, "Max speed the player keeps when releasing the pull (anti-slingshot)." );
static ConVar sk_barnacle_arrive_damp  ( "sk_barnacle_arrive_damp",   "0.3",  FCVAR_NONE, "Velocity fraction kept on arrival auto-detach (prevents ceiling pogo)." );
static ConVar sk_barnacle_stall_time   ( "sk_barnacle_stall_time",    "1.0",  FCVAR_NONE, "If pulling makes no progress for this long, auto-detach." );
static ConVar sk_barnacle_break_on_los ( "sk_barnacle_break_on_los",  "0",    FCVAR_NONE, "1 = tongue snaps when geometry blocks the line to the anchor (pre-wrapping behaviour)." );
static ConVar sk_barnacle_server_beam  ( "sk_barnacle_server_beam",   "0",    FCVAR_NONE, "1 = draw the legacy straight server beam from the eye. The client draws the real tongue now; this is a fallback only." );
static ConVar sk_barnacle_debug        ( "sk_barnacle_debug",         "0",    FCVAR_NONE, "1 = draw debug overlays and log state transitions." );

// Which surfaces the tongue can bite into.
//
// Matched against surfacedata_t::game.material, the single-character class
// every surfaceprop carries: F flesh, B bloodyflesh, H alienflesh, A antlion,
// O foliage, W wood, M metal, C concrete, and so on. Using the surfaceprop
// rather than the material name means a level designer marks geometry as
// organic the same way they already control its footsteps, impact effects and
// gibs - one $surfaceprop line in the VMT, no naming convention to remember.
static ConVar sk_barnacle_organic      ( "sk_barnacle_organic",       "FBHA", FCVAR_NONE, "Surfaceprop material chars the tongue may attach to. Empty = attach to anything." );

// Prey. A barnacle eats things, so a light physics object is a valid target
// whatever it is made of - a wooden crate reports surfaceprop 'W' and would
// otherwise be rejected by the organic rule above.
static ConVar sk_barnacle_eat_props    ( "sk_barnacle_eat_props",     "1",    FCVAR_NONE, "1 = light physics objects are always valid targets, ignoring the surface rule." );
static ConVar sk_barnacle_grab_mass    ( "sk_barnacle_grab_mass",     "150",  FCVAR_NONE, "Objects at or below this mass get reeled in. Heavier ones pull the player instead." );
static ConVar sk_barnacle_reel_speed   ( "sk_barnacle_reel_speed",    "600",  FCVAR_NONE, "Speed a reeled object is dragged at (units/sec)." );
static ConVar sk_barnacle_reel_accel   ( "sk_barnacle_reel_accel",    "2000", FCVAR_NONE, "How fast a reeled object's velocity converges (units/sec^2)." );
static ConVar sk_barnacle_crush_dist   ( "sk_barnacle_crush_dist",    "72",   FCVAR_NONE, "Distance at which a reeled object is bitten. Too small and the debris spawns behind the camera." );
static ConVar sk_barnacle_crush_damage ( "sk_barnacle_crush_damage",  "-1",   FCVAR_NONE, "Damage dealt when a reeled object arrives. -1 = exactly lethal for that object, avoiding overkill." );
static ConVar sk_barnacle_crush_dmgtype( "sk_barnacle_crush_dmgtype", "0",    FCVAR_NONE, "Damage type bits for the bite. 0 = DMG_CLUB." );
static ConVar sk_barnacle_crush_input  ( "sk_barnacle_crush_input",   "1",    FCVAR_NONE, "1 = bite by firing the target's own Break input and then removing it. 0 = damage only." );
static ConVar sk_barnacle_crush_force  ( "sk_barnacle_crush_force",   "600",  FCVAR_NONE, "Outward impulse applied to the debris, away from the player." );

// Per-entity tagging without a custom class, using response contexts - the
// closest thing Source has to Unreal's Tags array. Every CBaseEntity carries a
// key/value bag, settable in Hammer via the ResponseContext keyvalue and
// changeable at runtime through the stock AddContext / RemoveContext /
// ClearContext inputs. No subclass, no code, no FGD work.
//
// Put "barnacle:allow" in any brush entity or prop and the tongue bites it
// regardless of material; "barnacle:deny" and it never will.
//
// OFF BY DEFAULT, deliberately. The exact spelling of the context accessors
// moves between SDK branches - HasContext(), GetContextValue() and
// FindContextByName() are not all present everywhere - and a tagging
// convenience is not worth breaking a build over.
//
// To switch it on, find what your branch actually exposes:
//
//     findstr /n "Context" src\game\server\baseentity.h
//
// then set the define below to 1 and, if needed, adjust the single call in
// BarnacleGetEntityTag(). Everything else works either way.
#define BARNACLE_USE_RESPONSE_CONTEXTS  0
#define BARNACLE_CONTEXT_KEY            "barnacle"

enum BarnacleTag_t
{
	BARNACLE_TAG_NONE = 0,
	BARNACLE_TAG_ALLOW,
	BARNACLE_TAG_DENY,
};

//-----------------------------------------------------------------------------
// Reads the "barnacle" context off an entity.
//
// Matched on the first character rather than with a string compare, so that
// nothing outside the #if depends on any particular string library spelling.
// "allow" and "deny" are the documented values; anything else is ignored.
//-----------------------------------------------------------------------------
static BarnacleTag_t BarnacleGetEntityTag( CBaseEntity *pEnt )
{
#if BARNACLE_USE_RESPONSE_CONTEXTS
	if ( !pEnt )
		return BARNACLE_TAG_NONE;

	const char *pszValue = pEnt->GetContextValue( BARNACLE_CONTEXT_KEY );

	if ( !pszValue || !pszValue[0] )
		return BARNACLE_TAG_NONE;

	if ( pszValue[0] == 'd' || pszValue[0] == 'D' )
		return BARNACLE_TAG_DENY;

	if ( pszValue[0] == 'a' || pszValue[0] == 'A' )
		return BARNACLE_TAG_ALLOW;

	return BARNACLE_TAG_NONE;
#else
	return BARNACLE_TAG_NONE;
#endif
}

#define BARNACLE_BEAM_SPRITE   "sprites/laserbeam.vmt"   // stock asset
#endif

#ifdef CLIENT_DLL
//-----------------------------------------------------------------------------
// Client-only tunables (pure presentation).
//-----------------------------------------------------------------------------
static ConVar cl_barnacle_width        ( "cl_barnacle_width",         "2.5",  FCVAR_NONE, "Rendered thickness of the tongue." );
static ConVar cl_barnacle_corner_round ( "cl_barnacle_corner_round",  "12.0", FCVAR_NONE, "Corner rounding radius in units. 0 = sharp corners." );
static ConVar cl_barnacle_render       ( "cl_barnacle_render",        "1",    FCVAR_NONE, "0 = debug lines only, 1 = textured beam." );
static ConVar cl_barnacle_debug        ( "cl_barnacle_debug",         "0",    FCVAR_NONE, "1 = draw pivots and path overlays." );

// Where the tongue visually leaves the weapon.
//
// 0 = fixed offset in view space (default): a point just below the centre of
//     the screen, roughly where the gun sits. Reliable, tunable, and immune to
//     the view model FOV problem that makes attachment positions not line up
//     with the pixels you actually see.
// 1 = the model's "muzzle" attachment, falling back to the offset if the model
//     does not have one.
static ConVar cl_barnacle_origin_mode  ( "cl_barnacle_origin_mode",   "0",    FCVAR_NONE, "0 = view-space offset, 1 = muzzle attachment (offset as fallback)." );
static ConVar cl_barnacle_origin_fwd   ( "cl_barnacle_origin_fwd",    "14",   FCVAR_NONE, "Tongue origin: units forward from the eye." );
static ConVar cl_barnacle_origin_right ( "cl_barnacle_origin_right",  "6",    FCVAR_NONE, "Tongue origin: units right of the eye." );
static ConVar cl_barnacle_origin_up    ( "cl_barnacle_origin_up",     "-6",   FCVAR_NONE, "Tongue origin: units up from the eye. Negative = below screen centre." );

#define BARNACLE_TONGUE_MATERIAL  "sprites/laserbeam"	// no .vmt for materials->FindMaterial

#define BARNACLE_RENDER_PAD       16.0f	// slack added around the path's bounds

class C_BarnacleTongueRenderer;
#endif // CLIENT_DLL

#define BARNACLE_REFIRE_DELAY  0.4f

#ifdef CLIENT_DLL
#define CWeaponBarnacle C_WeaponBarnacle
#endif

//-----------------------------------------------------------------------------
// CWeaponBarnacle
//-----------------------------------------------------------------------------
class CWeaponBarnacle : public CBaseHL2MPCombatWeapon
{
public:
	DECLARE_CLASS( CWeaponBarnacle, CBaseHL2MPCombatWeapon );
	DECLARE_NETWORKCLASS();
	DECLARE_PREDICTABLE();

	CWeaponBarnacle( void );

	virtual void	Precache( void );
	virtual void	ItemPostFrame( void );
	virtual void	PrimaryAttack( void );
	virtual void	SecondaryAttack( void ) {}	// handled as a held button in ItemPostFrame
	virtual bool	Holster( CBaseCombatWeapon *pSwitchingTo = NULL );
	virtual bool	HasAnyAmmo( void ) { return true; }	// never "out of ammo"

	// State machine states. Networked (m_iState) so a future client-side
	// tongue visual / HUD element can key off it without code changes.
	enum BarnacleState_t
	{
		BARNACLE_IDLE = 0,		// nothing happening
		BARNACLE_FIRING,		// tongue tip is travelling forward
		BARNACLE_ATTACHED,		// tongue latched, player not being pulled
		BARNACLE_PULLING,		// tongue latched, player being pulled in
	};

	//-------------------------------------------------------------------------
	// Shared wrapping solver.
	//
	// Compiled into BOTH dlls and run independently by each: the server uses
	// the path to decide where to pull the player, the client uses it to draw
	// the tongue. Same algorithm, same collision data, so the two agree
	// without any of it being networked.
	//
	// The one deliberate difference is the free end - the server measures from
	// the eye (that is where it traced the shot from), the client measures from
	// the view model muzzle. They sit a few units apart, which at worst makes a
	// pivot appear one frame earlier on one side. Invisible in practice.
	//-------------------------------------------------------------------------
protected:
	bool			UpdatePivots( CBasePlayer *pOwner, const Vector &vecHand );
	void			RefreshPivotWorldPositions( void );
	void			AddPivot( const Vector &vecPos, const Vector &vecNormal, CBaseEntity *pEnt );
	bool			FindWrapCorner( const Vector &vecFrom, const Vector &vecTo,
									const trace_t &trForward, CBasePlayer *pOwner,
									Vector &vecCornerOut, Vector &vecNormalOut ) const;
	bool			IsClearPath( const Vector &a, const Vector &b, CBasePlayer *pOwner ) const;

	// Length of the whole polyline from vecHand back to the anchor. This is
	// the tongue's real length once it bends - straight-line distance
	// understates it badly around a corner.
	float			GetTonguePathLength( const Vector &vecHand ) const;

	// [0] = anchor (attach point) ... [n-1] = bend nearest the player.
	CUtlVector< BarnaclePivot_t >	m_Pivots;

public:

#ifndef CLIENT_DLL
	DECLARE_ACTTABLE();

	virtual void	Drop( const Vector &vecVelocity );

private:
	void			SetState( BarnacleState_t newState );
	const char		*StateName( int state ) const;

	// --- State handlers (one per state, dispatched from ItemPostFrame) --
	void			State_Idle( CBasePlayer *pOwner );
	void			State_Firing( CBasePlayer *pOwner, float dt );
	void			State_Attached( CBasePlayer *pOwner );
	void			State_Pulling( CBasePlayer *pOwner, float dt );

	// --- Actions --------------------------------------------------------
	void			StartFiring( CBasePlayer *pOwner );
	bool			TryAttach( const trace_t &tr );
	void			Detach( bool bKeepMomentum );

	// --- Helpers --------------------------------------------------------
	bool			IsSurfaceValid( const trace_t &tr ) const;
	bool			IsSurfaceOrganic( const trace_t &tr ) const;
	bool			IsReelableTarget( CBaseEntity *pEnt ) const;
	void			State_Reeling( CBasePlayer *pOwner, float dt );
	void			CrushTarget( CBasePlayer *pOwner, CBaseEntity *pTarget );
	bool			GetAttachWorldPos( Vector &out ) const;
	bool			IsAttachStillValid( CBasePlayer *pOwner );
	Vector			GetPullTarget( CBasePlayer *pOwner ) const;
	void			UpdateTongueVisual( CBasePlayer *pOwner, const Vector &vecTip );
	void			DestroyTongueVisual( void );

	// --- Server-only data ----------------------------------------------
	Vector			m_vecTongueTip;			// current tip position while extending
	Vector			m_vecTongueDir;			// locked-in direction at fire time
	Vector			m_vecTongueStart;		// where the tongue was fired from

	Vector			m_vecAttachPoint;		// world-space attach point (static geometry)
	bool			m_bAttachedToEntity;
	bool			m_bReelTarget;		// anchor is prey: drag it to us, not us to it
	EHANDLE			m_hAttachEntity;		// entity we latched onto (may move)
	Vector			m_vecAttachLocalOffset;	// attach point in entity-local space

	float			m_flClosestDist;		// best distance achieved this pull
	float			m_flLastProgressTime;	// last time we got measurably closer

	CHandle<CBeam>	m_hTongueBeam;
#endif // !CLIENT_DLL

#ifdef CLIENT_DLL
public:
	virtual void	OnDataChanged( DataUpdateType_t updateType );
	virtual void	ClientThink( void );
	virtual void	UpdateOnRemove( void );

	// Called by the renderer entity during the render pass.
	void			DrawTongue( void );
	bool			IsTongueVisible( void ) const { return m_bTongueActive; }
	bool			GetTongueRenderBounds( Vector &mins, Vector &maxs ) const;

private:
	// --- Per-frame driver ------------------------------------------------
	void			TongueStart( C_BasePlayer *pOwner );
	void			TongueStop( void );
	void			TongueUpdate( C_BasePlayer *pOwner, float dt );

	// --- Geometry / helpers ----------------------------------------------
	bool			GetTongueOrigin( CBasePlayer *pOwner, Vector &vecOut );
	void			BuildRenderPath( CUtlVector< Vector > &path ) const;

	// --- State -----------------------------------------------------------
	int				m_iPrevState;			// for edge detection on m_iState
	bool			m_bTongueActive;		// tongue currently drawn
	bool			m_bLatched;				// tip has reached its attach point

	Vector			m_vecEyeStart;			// where the trace originated (eye, matches server)
	Vector			m_vecFireDir;			// locked-in direction
	Vector			m_vecHand;				// muzzle position this frame (visual start)
	Vector			m_vecTip;				// current tip position

	float			m_flTravel;				// how far the tip has extended
	float			m_flHitDist;			// distance to the predicted impact
	Vector			m_vecHitPos;			// predicted impact point
	Vector			m_vecHitNormal;
	EHANDLE			m_hHitEnt;
	bool			m_bWillHit;

	CUtlVector< Vector >			m_RenderPath;

	CHandle< C_BarnacleTongueRenderer >	m_hRenderer;
	CMaterialReference					m_matTongue;
#endif // CLIENT_DLL

private:
	CNetworkVar( int, m_iState );

	CWeaponBarnacle( const CWeaponBarnacle & );	// not copyable
};

//-----------------------------------------------------------------------------
IMPLEMENT_NETWORKCLASS_ALIASED( WeaponBarnacle, DT_WeaponBarnacle )

BEGIN_NETWORK_TABLE( CWeaponBarnacle, DT_WeaponBarnacle )
#ifdef CLIENT_DLL
	RecvPropInt( RECVINFO( m_iState ) ),
#else
	SendPropInt( SENDINFO( m_iState ), 3, SPROP_UNSIGNED ),
#endif
END_NETWORK_TABLE()

#ifdef CLIENT_DLL
BEGIN_PREDICTION_DATA( CWeaponBarnacle )
END_PREDICTION_DATA()
#endif

LINK_ENTITY_TO_CLASS( weapon_barnacle, CWeaponBarnacle );
PRECACHE_WEAPON_REGISTER( weapon_barnacle );

#ifdef CLIENT_DLL
//=============================================================================
// C_BarnacleTongueRenderer
//
// A client-only entity whose sole job is to give us a draw call.
//
// Why this exists: in first person the weapon's own DrawModel() is never
// called (the view model is drawn instead), so there is no hook on the weapon
// itself to render world-space geometry from. The engine-standard answer is a
// client-side renderable registered in the leaf system - this is exactly how
// C_RopeKeyframe draws Source's own ropes.
//
// It owns no state: it asks the weapon for the path every frame.
//=============================================================================
class C_BarnacleTongueRenderer : public C_BaseEntity
{
	DECLARE_CLASS( C_BarnacleTongueRenderer, C_BaseEntity );

public:
	static C_BarnacleTongueRenderer *Create( CWeaponBarnacle *pWeapon, const Vector &vecOrigin )
	{
		C_BarnacleTongueRenderer *pRenderer = new C_BarnacleTongueRenderer;
		if ( !pRenderer )
			return NULL;

		// NULL model: we draw everything by hand in DrawModel().
		if ( !pRenderer->InitializeAsClientEntity( NULL, RENDER_GROUP_TRANSLUCENT_ENTITY ) )
		{
			pRenderer->Release();
			return NULL;
		}

		pRenderer->m_hWeapon = pWeapon;
		pRenderer->AddEffects( EF_NOSHADOW | EF_NORECEIVESHADOW );

		// A real position matters: the leaf system files this renderable into
		// world leaves by its origin and bounds, and a modelless entity left
		// at (0,0,0) gets drawn only while the leaf around the world origin is
		// visible. That is what made the tongue blink out at certain angles.
		pRenderer->SetAbsOrigin( vecOrigin );

		return pRenderer;
	}

	virtual bool ShouldDraw( void )
	{
		CWeaponBarnacle *pWeapon = m_hWeapon.Get();
		return ( pWeapon && pWeapon->IsTongueVisible() );
	}

	virtual RenderGroup_t GetRenderGroup( void )
	{
		return RENDER_GROUP_TRANSLUCENT_ENTITY;
	}

	// ONE bounds override, deliberately.
	//
	// This is the entity-space version, and it is the one that matters:
	// GetRenderBoundsWorldspace() derives itself from this by default, so
	// overriding only the world-space version - as an earlier revision did -
	// leaves the leaf system asking this one and getting a zero-size box. The
	// tongue then survived only while the leaf around the world origin was on
	// screen, which is what made it blink out at certain view angles.
	//
	// Returned relative to the render origin, which TongueUpdate() keeps on
	// the centre of the path. Moving the entity is also what re-files it into
	// the correct leaves: a position change invalidates that registration for
	// us, so there is no need to poke the leaf system by hand.
	virtual void GetRenderBounds( Vector &mins, Vector &maxs )
	{
		CWeaponBarnacle *pWeapon = m_hWeapon.Get();

		Vector vecWorldMins, vecWorldMaxs;

		if ( pWeapon && pWeapon->GetTongueRenderBounds( vecWorldMins, vecWorldMaxs ) )
		{
			const Vector vecOrigin = GetRenderOrigin();

			mins = vecWorldMins - vecOrigin;
			maxs = vecWorldMaxs - vecOrigin;
			return;
		}

		mins.Init( -BARNACLE_RENDER_PAD, -BARNACLE_RENDER_PAD, -BARNACLE_RENDER_PAD );
		maxs.Init(  BARNACLE_RENDER_PAD,  BARNACLE_RENDER_PAD,  BARNACLE_RENDER_PAD );
	}

	virtual int DrawModel( int flags )
	{
		CWeaponBarnacle *pWeapon = m_hWeapon.Get();
		if ( !pWeapon || !pWeapon->IsTongueVisible() )
			return 0;

		pWeapon->DrawTongue();
		return 1;
	}

private:
	CHandle< CWeaponBarnacle > m_hWeapon;
};
#endif // CLIENT_DLL

#ifndef CLIENT_DLL
// Standard HL2MP third-person activity table (crossbow set, since we borrow
// its models anyway).
acttable_t CWeaponBarnacle::m_acttable[] =
{
	{ ACT_HL2MP_IDLE,					ACT_HL2MP_IDLE_CROSSBOW,					false },
	{ ACT_HL2MP_RUN,					ACT_HL2MP_RUN_CROSSBOW,						false },
	{ ACT_HL2MP_IDLE_CROUCH,			ACT_HL2MP_IDLE_CROUCH_CROSSBOW,				false },
	{ ACT_HL2MP_WALK_CROUCH,			ACT_HL2MP_WALK_CROUCH_CROSSBOW,				false },
	{ ACT_HL2MP_GESTURE_RANGE_ATTACK,	ACT_HL2MP_GESTURE_RANGE_ATTACK_CROSSBOW,	false },
	{ ACT_HL2MP_GESTURE_RELOAD,			ACT_HL2MP_GESTURE_RELOAD_CROSSBOW,			false },
	{ ACT_HL2MP_JUMP,					ACT_HL2MP_JUMP_CROSSBOW,					false },
};
IMPLEMENT_ACTTABLE( CWeaponBarnacle );
#endif

//-----------------------------------------------------------------------------
CWeaponBarnacle::CWeaponBarnacle( void )
{
	m_iState = BARNACLE_IDLE;

#ifndef CLIENT_DLL
	m_bAttachedToEntity  = false;
	m_bReelTarget        = false;
	m_flClosestDist      = 0.0f;
	m_flLastProgressTime = 0.0f;
#endif

#ifdef CLIENT_DLL
	m_iPrevState    = BARNACLE_IDLE;
	m_bTongueActive = false;
	m_bLatched      = false;
	m_bWillHit      = false;
	m_flTravel      = 0.0f;
	m_flHitDist     = 0.0f;

	m_vecEyeStart.Init();
	m_vecFireDir.Init();
	m_vecHand.Init();
	m_vecTip.Init();
	m_vecHitPos.Init();
	m_vecHitNormal.Init();
#endif

	m_bFiresUnderwater = true;
	m_fMinRange1       = 0;
	m_fMaxRange1       = 0;		// NPCs never use this weapon
}

//-----------------------------------------------------------------------------
void CWeaponBarnacle::Precache( void )
{
#ifndef CLIENT_DLL
	PrecacheModel( BARNACLE_BEAM_SPRITE );
#endif
	BaseClass::Precache();
}

//-----------------------------------------------------------------------------
// Main per-frame dispatch.
//
// We deliberately do NOT call BaseClass::ItemPostFrame() - the base
// implementation owns primary/secondary attack scheduling and reload logic,
// which would fight our state machine. All input is read explicitly here.
//
// Client side intentionally does nothing: no prediction for the prototype
// (see architecture note at the top of the file).
//-----------------------------------------------------------------------------
void CWeaponBarnacle::ItemPostFrame( void )
{
#ifndef CLIENT_DLL
	CBasePlayer *pOwner = ToBasePlayer( GetOwner() );
	if ( !pOwner )
		return;

	const float dt = gpGlobals->frametime;

	switch ( m_iState )
	{
	case BARNACLE_IDLE:     State_Idle( pOwner );          break;
	case BARNACLE_FIRING:   State_Firing( pOwner, dt );    break;
	case BARNACLE_ATTACHED: State_Attached( pOwner );      break;
	case BARNACLE_PULLING:  State_Pulling( pOwner, dt );   break;
	}

	WeaponIdle();
#endif
}

//-----------------------------------------------------------------------------
void CWeaponBarnacle::PrimaryAttack( void )
{
#ifndef CLIENT_DLL
	CBasePlayer *pOwner = ToBasePlayer( GetOwner() );
	if ( !pOwner )
		return;

	if ( m_iState == BARNACLE_IDLE )
	{
		StartFiring( pOwner );
	}
	else if ( m_iState == BARNACLE_ATTACHED || m_iState == BARNACLE_PULLING )
	{
		// Primary while latched = manual cancel.
		Detach( true );
	}
#endif
}

//-----------------------------------------------------------------------------
bool CWeaponBarnacle::Holster( CBaseCombatWeapon *pSwitchingTo )
{
#ifndef CLIENT_DLL
	Detach( true );
#endif
	return BaseClass::Holster( pSwitchingTo );
}

//=============================================================================
// WRAPPING  (shared - compiled into both dlls)
//
// The tongue path is a polyline: m_Pivots[0] is the anchor, the tail is the
// bend closest to the player. Two operations:
//
//   unwrap - the last bend stops carrying load the instant the free end can
//            see past it to the node behind it. That is the moment tension at
//            that corner disappears, so that is the moment it is removed -
//            not some distance threshold.
//   wrap   - if the free end can no longer see the last node, geometry got in
//            between and a new bend goes on its edge.
//
// Both run to convergence rather than once. Rounding a corner frees the bend,
// which can immediately expose a different corner behind it; a single pass
// would leave the path one frame stale and, worse, leave the pull aimed at a
// node that is no longer holding anything.
//
// Returns true if the path changed, so callers know to re-derive anything
// they computed from it.
//=============================================================================
bool CWeaponBarnacle::UpdatePivots( CBasePlayer *pOwner, const Vector &vecHand )
{
	if ( m_Pivots.Count() == 0 )
		return false;	// not anchored yet - nothing to wrap around

	bool bChanged = false;

	// Bends released during this call. Belt and braces on top of the matched
	// tests below: whatever the geometry does, a bend that was just let go is
	// not recreated in the same place on the same call.
	CUtlVector< Vector > released;

	// Bounded: each iteration must either remove or add a bend, and both are
	// capped, so this cannot spin. The guard is for pathological geometry.
	for ( int iPass = 0; iPass < BARNACLE_SOLVER_PASSES; ++iPass )
	{
		bool bPassChanged = false;

		// ---- unwrap -------------------------------------------------------
		while ( m_Pivots.Count() > 1 )
		{
			const Vector vecTail = m_Pivots.Tail().vecPos;
			const Vector vecPrev = m_Pivots[ m_Pivots.Count() - 2 ].vecPos;

			// Deliberately UNBIASED, and this is the important part.
			//
			// Removing the tail makes vecPrev the new tail, and the wrap pass
			// below immediately traces exactly this segment. So the two tests
			// must be the same test. Give unwrap any clearance the wrap pass
			// does not get - the intuitive "nudge it away from the corner"
			// hysteresis - and you open a band where unwrap says clear and
			// wrap says blocked, in the same frame, on the same line. The bend
			// is then removed and recreated forever and the pull parks on that
			// corner and never rounds it.
			//
			// Identical tests cannot contradict each other. Any residual
			// flicker at the exact boundary is harmless, because at the
			// boundary the bend and the direct path coincide anyway.
			if ( !IsClearPath( vecHand, vecPrev, pOwner ) )
				break;

			released.AddToTail( vecTail );
			m_Pivots.Remove( m_Pivots.Count() - 1 );
			bPassChanged = true;

#ifndef CLIENT_DLL
			if ( sk_barnacle_debug.GetBool() )
				DevMsg( "weapon_barnacle: bend released, %d left\n", m_Pivots.Count() );
#endif
		}

		// ---- wrap ---------------------------------------------------------
		for ( int i = 0; i < BARNACLE_WRAPS_PER_FRAME; ++i )
		{
			if ( m_Pivots.Count() >= BARNACLE_MAX_PIVOTS )
				break;

			const Vector vecLast = m_Pivots.Tail().vecPos;

			trace_t tr;
			UTIL_TraceLine( vecHand, vecLast, BARNACLE_WRAP_MASK, pOwner, COLLISION_GROUP_NONE, &tr );

			if ( tr.fraction >= 1.0f )
				break;	// clear line to the last node - nothing to wrap

			// Degenerate start: the free end is inside geometry, so the trace
			// tells us nothing about where an edge is. Adding a bend from it
			// produces a garbage position. Wait for the next frame instead.
			if ( tr.startsolid || tr.allsolid )
				break;

			Vector vecCorner, vecNormal;
			if ( !FindWrapCorner( vecHand, vecLast, tr, pOwner, vecCorner, vecNormal ) )
				break;	// could not resolve an edge; let it clip rather than flicker

			bool bJustReleased = false;
			for ( int j = 0; j < released.Count(); ++j )
			{
				if ( ( released[j] - vecCorner ).LengthSqr() < BARNACLE_REWRAP_GUARD * BARNACLE_REWRAP_GUARD )
				{
					bJustReleased = true;
					break;
				}
			}

			if ( bJustReleased )
				break;	// we just let this corner go; do not grab it again

			AddPivot( vecCorner, vecNormal, tr.m_pEnt );
			bPassChanged = true;

#ifndef CLIENT_DLL
			if ( sk_barnacle_debug.GetBool() )
				DevMsg( "weapon_barnacle: bend added, %d total\n", m_Pivots.Count() );
#endif
		}

		if ( !bPassChanged )
			break;		// stable

		bChanged = true;
	}

	return bChanged;
}

//-----------------------------------------------------------------------------
// Find the edge to wrap around.
//
// tr.endpos is NOT the corner - it is where the line hit a face. The corner is
// where two faces meet. Solve it exactly as the intersection of three planes:
//
//     n1 . X = n1 . p1     the face hit going hand -> pivot
//     n2 . X = n2 . p2     the face hit going pivot -> hand
//     n3 . X = n3 . a      the plane the tongue itself lies in
//
// This is exact for a convex edge between two brush faces, which is the case
// that matters. Everything else (thin walls, pillars, both traces landing on
// the same face) falls through to a short march along the bracket.
//-----------------------------------------------------------------------------
bool CWeaponBarnacle::FindWrapCorner( const Vector &vecFrom, const Vector &vecTo,
									  const trace_t &trForward, CBasePlayer *pOwner,
									  Vector &vecCornerOut, Vector &vecNormalOut ) const
{
	trace_t trBack;
	UTIL_TraceLine( vecTo, vecFrom, BARNACLE_WRAP_MASK, pOwner, COLLISION_GROUP_NONE, &trBack );

	const Vector n1 = trForward.plane.normal;
	const Vector p1 = trForward.endpos;
	const Vector n2 = trBack.plane.normal;
	const Vector p2 = trBack.endpos;

	// Direction to push the pivot off the surface so the tongue does not sink
	// into the corner it is wrapping.
	Vector vecAvgNormal = n1 + n2;
	if ( VectorNormalize( vecAvgNormal ) < 0.01f )
		vecAvgNormal = n1;

	// --- exact three-plane solve -------------------------------------------
	if ( trBack.fraction < 1.0f )
	{
		Vector n3 = CrossProduct( vecTo - vecFrom, p1 - vecFrom );
		if ( VectorNormalize( n3 ) > 0.01f )
		{
			const float d1 = DotProduct( n1, p1 );
			const float d2 = DotProduct( n2, p2 );
			const float d3 = DotProduct( n3, vecFrom );

			const Vector c23 = CrossProduct( n2, n3 );
			const Vector c31 = CrossProduct( n3, n1 );
			const Vector c12 = CrossProduct( n1, n2 );

			const float det = DotProduct( n1, c23 );
			if ( fabs( det ) > 1e-4f )
			{
				const Vector vecEdge = ( c23 * d1 + c31 * d2 + c12 * d3 ) / det;

				// Near-parallel faces send the intersection off to infinity.
				if ( vecEdge.IsValid() && ( vecEdge - p1 ).Length() < BARNACLE_CORNER_MAX_DIST )
				{
					const Vector vecCandidate = vecEdge + vecAvgNormal * BARNACLE_PIVOT_OFFSET;

					if ( IsClearPath( vecCandidate, vecFrom, pOwner ) &&
						 IsClearPath( vecCandidate, vecTo,   pOwner ) )
					{
						vecCornerOut = vecCandidate;
						vecNormalOut = vecAvgNormal;
						return true;
					}
				}
			}
		}
	}

	// --- fallback march ----------------------------------------------------
	// Walk the bracket p1..p2 and take the first offset point that can see
	// both ends. Only runs on the frame a pivot is created, so the extra
	// traces are not a concern.
	const Vector vecSpan = p2 - p1;
	for ( int i = 0; i <= BARNACLE_CORNER_STEPS; ++i )
	{
		const float t = (float)i / (float)BARNACLE_CORNER_STEPS;
		const Vector vecCandidate = p1 + vecSpan * t + vecAvgNormal * BARNACLE_PIVOT_OFFSET;

		if ( IsClearPath( vecCandidate, vecFrom, pOwner ) &&
			 IsClearPath( vecCandidate, vecTo,   pOwner ) )
		{
			vecCornerOut = vecCandidate;
			vecNormalOut = vecAvgNormal;
			return true;
		}
	}

	return false;
}

//-----------------------------------------------------------------------------
bool CWeaponBarnacle::IsClearPath( const Vector &a, const Vector &b, CBasePlayer *pOwner ) const
{
	trace_t tr;
	UTIL_TraceLine( a, b, BARNACLE_WRAP_MASK, pOwner, COLLISION_GROUP_NONE, &tr );

	// A trace that starts inside solid reports fraction 0, which would read as
	// "blocked" and permanently wedge the solver: it could neither unwrap a
	// bend nor resolve a new one. Treat it as unknown-but-passable so the
	// path can still relax once the reference point is back in open space.
	if ( tr.startsolid || tr.allsolid )
		return true;

	return ( tr.fraction >= 1.0f );
}

//-----------------------------------------------------------------------------
void CWeaponBarnacle::AddPivot( const Vector &vecPos, const Vector &vecNormal, CBaseEntity *pEnt )
{
	BarnaclePivot_t pivot;
	pivot.vecPos    = vecPos;
	pivot.vecNormal = vecNormal;
	pivot.vecLocal  = vec3_origin;
	pivot.hEnt      = NULL;

	// entindex 0 is worldspawn: static, no need to track it.
	if ( pEnt && pEnt->entindex() > 0 )
	{
		pivot.hEnt = pEnt;
		VectorITransform( vecPos, pEnt->EntityToWorldTransform(), pivot.vecLocal );
	}

	m_Pivots.AddToTail( pivot );
}

//-----------------------------------------------------------------------------
void CWeaponBarnacle::RefreshPivotWorldPositions( void )
{
	for ( int i = 0; i < m_Pivots.Count(); ++i )
	{
		CBaseEntity *pEnt = m_Pivots[i].hEnt.Get();
		if ( pEnt )
			VectorTransform( m_Pivots[i].vecLocal, pEnt->EntityToWorldTransform(), m_Pivots[i].vecPos );
	}
}

//-----------------------------------------------------------------------------
// Total length of the tongue along its bends.
//-----------------------------------------------------------------------------
float CWeaponBarnacle::GetTonguePathLength( const Vector &vecHand ) const
{
	if ( m_Pivots.Count() == 0 )
		return 0.0f;

	float flLength = ( m_Pivots.Tail().vecPos - vecHand ).Length();

	for ( int i = m_Pivots.Count() - 1; i > 0; --i )
		flLength += ( m_Pivots[i - 1].vecPos - m_Pivots[i].vecPos ).Length();

	return flLength;
}

#ifndef CLIENT_DLL
//=============================================================================
// Everything below is server-only gameplay logic.
//=============================================================================

//=============================================================================
//
// trigger_barnacle_attach
//
// A brush volume that changes whether the tongue may bite inside it, and can
// be switched on and off at runtime.
//
// ONE entity covers both roles, chosen by the Mode keyvalue:
//
//   Mode 0 (Deny)  - nothing can be attached to inside this volume while the
//                    volume is enabled, organic or not. This is the lockout.
//   Mode 1 (Allow) - anything inside may be attached to, bypassing the
//                    surface material rule. This is the whitelist, for making
//                    a crate or a pipe grabbable without authoring a VMT.
//
// Deny beats Allow when volumes overlap, so a lockout cannot be defeated by
// stacking a permissive volume on top of it.
//
// Design note on why this is a bespoke entity rather than a trigger_multiple:
// Source triggers are touch-driven, and a tongue impact point is not an
// entity, so there is nothing to touch them. What is needed is a point query
// at the moment of attaching, which means keeping a list and testing the
// point directly. That is what the static registry below is for - there are
// only ever a handful of these per map, so a linear walk is free.
//
// It lives in this file purely so no new source file has to be added to the
// VPC. Split it out whenever that stops being a convenience.
//
//=============================================================================
enum BarnacleZoneMode_t
{
	BARNACLE_ZONE_MODE_DENY = 0,
	BARNACLE_ZONE_MODE_ALLOW,
};

enum BarnacleZoneResult_t
{
	BARNACLE_ZONE_NONE = 0,		// no volume had an opinion - normal rules apply
	BARNACLE_ZONE_ALLOW,		// a whitelist volume said yes
	BARNACLE_ZONE_DENY,			// a lockout volume said no
};

class CBarnacleAttachZone : public CBaseEntity
{
	DECLARE_CLASS( CBarnacleAttachZone, CBaseEntity );
	DECLARE_DATADESC();

public:
	virtual void	Spawn( void );
	virtual void	UpdateOnRemove( void );

	bool			IsActive( void ) const	{ return !m_bDisabled; }
	int				GetMode( void ) const	{ return m_iMode; }
	bool			ShouldBreakExisting( void ) const { return m_bBreakExisting; }
	string_t		GetZoneTag( void ) const { return m_iszZoneTag; }

	bool			ContainsPoint( const Vector &vecPoint );

	void			InputEnable( inputdata_t &inputdata );
	void			InputDisable( inputdata_t &inputdata );
	void			InputToggle( inputdata_t &inputdata );

	void			FireAttachEvent( bool bAllowed, CBaseEntity *pActivator );

	static CUtlVector< CBarnacleAttachZone * > s_Zones;

private:
	bool			m_bDisabled;
	int				m_iMode;
	bool			m_bBreakExisting;
	string_t		m_iszZoneTag;

	COutputEvent	m_OnAttachAllowed;
	COutputEvent	m_OnAttachBlocked;
};

CUtlVector< CBarnacleAttachZone * > CBarnacleAttachZone::s_Zones;

LINK_ENTITY_TO_CLASS( trigger_barnacle_attach, CBarnacleAttachZone );

BEGIN_DATADESC( CBarnacleAttachZone )
	DEFINE_KEYFIELD( m_bDisabled,		FIELD_BOOLEAN,	"StartDisabled" ),
	DEFINE_KEYFIELD( m_iMode,			FIELD_INTEGER,	"Mode" ),
	DEFINE_KEYFIELD( m_bBreakExisting,	FIELD_BOOLEAN,	"BreakExisting" ),
	DEFINE_KEYFIELD( m_iszZoneTag,		FIELD_STRING,	"ZoneTag" ),

	DEFINE_INPUTFUNC( FIELD_VOID, "Enable",  InputEnable ),
	DEFINE_INPUTFUNC( FIELD_VOID, "Disable", InputDisable ),
	DEFINE_INPUTFUNC( FIELD_VOID, "Toggle",  InputToggle ),

	DEFINE_OUTPUT( m_OnAttachAllowed, "OnAttachAllowed" ),
	DEFINE_OUTPUT( m_OnAttachBlocked, "OnAttachBlocked" ),
END_DATADESC()

void CBarnacleAttachZone::Spawn( void )
{
	BaseClass::Spawn();

	// Standard non-solid brush volume: present for point queries, invisible,
	// and never in the way of anything.
	SetSolid( SOLID_BSP );
	AddSolidFlags( FSOLID_NOT_SOLID );
	SetMoveType( MOVETYPE_NONE );
	SetModel( STRING( GetModelName() ) );
	AddEffects( EF_NODRAW );

	if ( s_Zones.Find( this ) == s_Zones.InvalidIndex() )
		s_Zones.AddToTail( this );

	// Unconditional, but DevMsg only surfaces at developer 1. If a zone is
	// silently not spawning - wrong classname, brushes stripped at compile,
	// entity culled - this line is the fastest way to find out.
	DevMsg( "trigger_barnacle_attach: '%s' spawned, mode %s, %s, model '%s' (%d zones)\n",
			GetEntityName().ToCStr(),
			( m_iMode == BARNACLE_ZONE_MODE_DENY ) ? "DENY" : "ALLOW",
			m_bDisabled ? "disabled" : "active",
			GetModelName().ToCStr(),
			s_Zones.Count() );
}

void CBarnacleAttachZone::UpdateOnRemove( void )
{
	s_Zones.FindAndRemove( this );
	BaseClass::UpdateOnRemove();
}

//-----------------------------------------------------------------------------
bool CBarnacleAttachZone::ContainsPoint( const Vector &vecPoint )
{
	// A degenerate ray clipped against this entity's own collideable:
	// startsolid means the point is inside the brush.
	//
	// This is exactly how CBaseTrigger::PointIsWithin() does it, and it is
	// used here for the same reason Valve uses it - unlike a contents query
	// it does not depend on which compile flags the trigger material ended up
	// with, and unlike a bounds test it respects the shape the level designer
	// actually drew.
	Ray_t ray;
	trace_t tr;

	ray.Init( vecPoint, vecPoint );
	enginetrace->ClipRayToCollideable( ray, MASK_ALL, GetCollideable(), &tr );

	return tr.startsolid;
}

void CBarnacleAttachZone::InputEnable( inputdata_t &inputdata )		{ m_bDisabled = false; }
void CBarnacleAttachZone::InputDisable( inputdata_t &inputdata )		{ m_bDisabled = true; }
void CBarnacleAttachZone::InputToggle( inputdata_t &inputdata )		{ m_bDisabled = !m_bDisabled; }

void CBarnacleAttachZone::FireAttachEvent( bool bAllowed, CBaseEntity *pActivator )
{
	if ( bAllowed )
		m_OnAttachAllowed.FireOutput( pActivator, this );
	else
		m_OnAttachBlocked.FireOutput( pActivator, this );
}

//-----------------------------------------------------------------------------
// Ask every active volume about a point. Deny short-circuits.
//
// pFiringZone, if given, receives the volume that decided, so the caller can
// fire its output without walking the list a second time.
//-----------------------------------------------------------------------------
static BarnacleZoneResult_t BarnacleQueryZones( const Vector &vecPoint,
												bool bBreakingOnly = false,
												CBarnacleAttachZone **ppDecidingZone = NULL )
{
	BarnacleZoneResult_t result = BARNACLE_ZONE_NONE;

	if ( ppDecidingZone )
		*ppDecidingZone = NULL;

	const bool bDebug = sk_barnacle_debug.GetBool();

	if ( bDebug )
		DevMsg( "weapon_barnacle: testing point against %d zone(s)\n", CBarnacleAttachZone::s_Zones.Count() );

	for ( int i = 0; i < CBarnacleAttachZone::s_Zones.Count(); ++i )
	{
		CBarnacleAttachZone *pZone = CBarnacleAttachZone::s_Zones[i];

		if ( !pZone )
			continue;

		if ( bDebug )
		{
			DevMsg( "  '%s': %s, mode %s, contains point: %s\n",
					pZone->GetEntityName().ToCStr(),
					pZone->IsActive() ? "active" : "disabled",
					( pZone->GetMode() == BARNACLE_ZONE_MODE_DENY ) ? "DENY" : "ALLOW",
					pZone->ContainsPoint( vecPoint ) ? "YES" : "no" );
		}

		if ( !pZone->IsActive() )
			continue;

		// When re-validating an existing tongue we only care about volumes
		// that are configured to cut one that is already attached.
		if ( bBreakingOnly && !pZone->ShouldBreakExisting() )
			continue;

		if ( !pZone->ContainsPoint( vecPoint ) )
			continue;

		if ( pZone->GetMode() == BARNACLE_ZONE_MODE_DENY )
		{
			if ( ppDecidingZone )
				*ppDecidingZone = pZone;

			return BARNACLE_ZONE_DENY;		// a lockout cannot be outvoted
		}

		result = BARNACLE_ZONE_ALLOW;

		if ( ppDecidingZone )
			*ppDecidingZone = pZone;
	}

	return result;
}

void CWeaponBarnacle::Drop( const Vector &vecVelocity )
{
	Detach( true );
	BaseClass::Drop( vecVelocity );
}

//-----------------------------------------------------------------------------
// State machine bookkeeping
//-----------------------------------------------------------------------------
const char *CWeaponBarnacle::StateName( int state ) const
{
	switch ( state )
	{
	case BARNACLE_IDLE:     return "IDLE";
	case BARNACLE_FIRING:   return "FIRING";
	case BARNACLE_ATTACHED: return "ATTACHED";
	case BARNACLE_PULLING:  return "PULLING";
	}
	return "<unknown>";
}

void CWeaponBarnacle::SetState( BarnacleState_t newState )
{
	if ( m_iState == newState )
		return;

	if ( sk_barnacle_debug.GetBool() )
	{
		DevMsg( "weapon_barnacle: %s -> %s\n", StateName( m_iState ), StateName( newState ) );
	}

	// Per-transition setup
	if ( newState == BARNACLE_PULLING )
	{
		m_flClosestDist      = 999999.0f;
		m_flLastProgressTime = gpGlobals->curtime;
	}

	m_iState = newState;
}

//-----------------------------------------------------------------------------
// IDLE: wait for the fire button.
//-----------------------------------------------------------------------------
void CWeaponBarnacle::State_Idle( CBasePlayer *pOwner )
{
	if ( ( pOwner->m_afButtonPressed & IN_ATTACK ) && m_flNextPrimaryAttack <= gpGlobals->curtime )
	{
		PrimaryAttack();
	}
}

void CWeaponBarnacle::StartFiring( CBasePlayer *pOwner )
{
	Vector vecDir;
	pOwner->EyeVectors( &vecDir );

	m_vecTongueStart = pOwner->Weapon_ShootPosition();
	m_vecTongueTip   = m_vecTongueStart;
	m_vecTongueDir   = vecDir;

	m_Pivots.RemoveAll();

	WeaponSound( SINGLE );
	SendWeaponAnim( ACT_VM_PRIMARYATTACK );
	pOwner->SetAnimation( PLAYER_ATTACK1 );

	m_flNextPrimaryAttack = gpGlobals->curtime + BARNACLE_REFIRE_DELAY;

	SetState( BARNACLE_FIRING );
}

//-----------------------------------------------------------------------------
// FIRING: the tongue tip travels forward at a fixed speed. Each frame we
// sweep-trace only the segment covered this frame, so a fast tip cannot
// tunnel through thin geometry.
//-----------------------------------------------------------------------------
void CWeaponBarnacle::State_Firing( CBasePlayer *pOwner, float dt )
{
	const float flStep   = sk_barnacle_tongue_speed.GetFloat() * dt;
	const Vector vecNext = m_vecTongueTip + m_vecTongueDir * flStep;

	trace_t tr;
	UTIL_TraceLine( m_vecTongueTip, vecNext, MASK_SOLID, pOwner, COLLISION_GROUP_NONE, &tr );

	if ( tr.fraction < 1.0f )
	{
		// Hit something. Attach or retract depending on validity.
		if ( IsSurfaceValid( tr ) && TryAttach( tr ) )
		{
			m_vecTongueTip = tr.endpos;
			UpdateTongueVisual( pOwner, m_vecTongueTip );
			SetState( BARNACLE_ATTACHED );
		}
		else
		{
			// Requirement: invalid surface cancels the action -> IDLE.
			if ( sk_barnacle_debug.GetBool() )
				NDebugOverlay::Cross3D( tr.endpos, 6.0f, 255, 64, 64, true, 1.0f );
			Detach( false );
		}
		return;
	}

	m_vecTongueTip = vecNext;

	// Safety check: distance limit - the tongue does not have unlimited range.
	if ( ( m_vecTongueTip - m_vecTongueStart ).Length() >= sk_barnacle_range.GetFloat() )
	{
		Detach( false );
		return;
	}

	UpdateTongueVisual( pOwner, m_vecTongueTip );
}

//-----------------------------------------------------------------------------
// Surface / entity validity rules:
//   - sky is never valid
//   - the owner is never valid
//   - everything else (world, displacements, props, NPCs, players) is fair
//     game for the prototype. Tightening this to an Opposing-Force-style
//     whitelist (e.g. only materials under materials/grapple/, via
//     tr.surface.name) is a one-line change here.
//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------
// Is this surface organic enough to bite?
//
// Read off the surfaceprop, not the material name. Every surfaceprop carries a
// single-character class in surfacedata_t::game.material, which is what the
// engine already uses to pick footstep sounds and impact effects - so marking
// geometry as grabbable is one $surfaceprop line in the VMT, and it stays
// consistent with how that geometry already behaves.
//-----------------------------------------------------------------------------
bool CWeaponBarnacle::IsSurfaceOrganic( const trace_t &tr ) const
{
	const char *pszAllowed = sk_barnacle_organic.GetString();

	// Empty list disables the rule entirely - useful while blocking out a map
	// before any organic materials exist.
	if ( !pszAllowed || !pszAllowed[0] )
		return true;

	surfacedata_t *pSurface = physprops->GetSurfaceData( tr.surface.surfaceProps );
	if ( !pSurface )
		return false;

	const char chMaterial = pSurface->game.material;

	for ( const char *pszCursor = pszAllowed; *pszCursor; ++pszCursor )
	{
		if ( *pszCursor == chMaterial )
			return true;
	}

	return false;
}

//-----------------------------------------------------------------------------
// Is this thing prey - something the tongue drags in rather than something the
// tongue drags us to?
//
// Mass is the whole rule, which means a level designer tunes it the way they
// already tune everything else about a prop. Anything heavier than
// sk_barnacle_grab_mass simply becomes an anchor instead.
//-----------------------------------------------------------------------------
bool CWeaponBarnacle::IsReelableTarget( CBaseEntity *pEnt ) const
{
	if ( !pEnt || pEnt->IsWorld() || pEnt->IsPlayer() )
		return false;

	if ( pEnt->GetMoveType() != MOVETYPE_VPHYSICS )
		return false;

	IPhysicsObject *pPhys = pEnt->VPhysicsGetObject();

	if ( !pPhys || !pPhys->IsMoveable() )
		return false;

	return ( pPhys->GetMass() <= sk_barnacle_grab_mass.GetFloat() );
}

//-----------------------------------------------------------------------------
// Attach rules, in priority order:
//
//   1. never the sky, never the owner
//   2. an active Deny volume vetoes everything
//   3. an active Allow volume overrides the material rule
//   4. otherwise the surface must be organic
//-----------------------------------------------------------------------------
bool CWeaponBarnacle::IsSurfaceValid( const trace_t &tr ) const
{
	if ( tr.surface.flags & SURF_SKY )
		return false;

	if ( tr.m_pEnt && tr.m_pEnt == GetOwner() )
		return false;

	// Per-entity tagging, checked before volumes so a tagged object wins
	// wherever it happens to be standing. Compiled out unless
	// BARNACLE_USE_RESPONSE_CONTEXTS is on.
	const BarnacleTag_t tag = BarnacleGetEntityTag( tr.m_pEnt );

	if ( tag == BARNACLE_TAG_DENY )
		return false;

	if ( tag == BARNACLE_TAG_ALLOW )
		return true;

	CBarnacleAttachZone *pZone = NULL;
	const BarnacleZoneResult_t zone = BarnacleQueryZones( tr.endpos, false, &pZone );

	if ( zone == BARNACLE_ZONE_DENY )
	{
		if ( pZone )
			pZone->FireAttachEvent( false, GetOwner() );

		if ( sk_barnacle_debug.GetBool() )
			DevMsg( "weapon_barnacle: attach denied by zone\n" );

		return false;
	}

	if ( zone == BARNACLE_ZONE_ALLOW )
	{
		if ( pZone )
			pZone->FireAttachEvent( true, GetOwner() );

		return true;
	}

	// Prey is fair game whatever it is made of.
	if ( sk_barnacle_eat_props.GetBool() && IsReelableTarget( tr.m_pEnt ) )
		return true;

	if ( !IsSurfaceOrganic( tr ) )
	{
		if ( sk_barnacle_debug.GetBool() )
			DevMsg( "weapon_barnacle: surface not organic (%s)\n", tr.surface.name ? tr.surface.name : "?" );

		return false;
	}

	return true;
}

bool CWeaponBarnacle::TryAttach( const trace_t &tr )
{
	m_vecAttachPoint    = tr.endpos;
	m_bAttachedToEntity = false;
	m_hAttachEntity     = NULL;

	CBaseEntity *pEnt = tr.m_pEnt;
	if ( pEnt && !pEnt->IsWorld() )
	{
		// Latched onto an entity: remember the attach point in entity-local
		// space so the point follows the entity if it moves.
		m_bAttachedToEntity = true;
		m_hAttachEntity     = pEnt;
		VectorITransform( tr.endpos, pEnt->EntityToWorldTransform(), m_vecAttachLocalOffset );
	}

	m_bReelTarget = IsReelableTarget( pEnt );

	// The anchor is also pivot[0] - the far end of the wrap path. Everything
	// added after it is a bend between here and the player.
	m_Pivots.RemoveAll();
	AddPivot( tr.endpos, tr.plane.normal, pEnt );

	return true;
}

//-----------------------------------------------------------------------------
// Resolve the current world-space attach position (handles moving entities).
// Returns false if the attachment no longer exists.
//-----------------------------------------------------------------------------
bool CWeaponBarnacle::GetAttachWorldPos( Vector &out ) const
{
	if ( m_bAttachedToEntity )
	{
		CBaseEntity *pEnt = m_hAttachEntity.Get();
		if ( !pEnt )
			return false;	// entity was removed / killed

		VectorTransform( m_vecAttachLocalOffset, pEnt->EntityToWorldTransform(), out );
		return true;
	}

	out = m_vecAttachPoint;
	return true;
}

//-----------------------------------------------------------------------------
// Shared validity checks while latched (ATTACHED and PULLING):
//   - attach entity still alive
//   - still within (slightly padded) range
//   - line of sight to the attach point not blocked by other geometry
//-----------------------------------------------------------------------------
bool CWeaponBarnacle::IsAttachStillValid( CBasePlayer *pOwner )
{
	Vector vecAttach;
	if ( !GetAttachWorldPos( vecAttach ) )
		return false;

	// A lockout volume switched on while you are hanging cuts the tongue. That
	// is the whole point of the button: it has to matter while attached, not
	// only at the moment of firing.
	if ( BarnacleQueryZones( vecAttach, true ) == BARNACLE_ZONE_DENY )
	{
		if ( sk_barnacle_debug.GetBool() )
			DevMsg( "weapon_barnacle: anchor entered an active deny zone, releasing\n" );

		return false;
	}

	const Vector vecEye = pOwner->EyePosition();

	// Padded range check. Measured along the bends: a tongue wrapped twice
	// around a pillar is much longer than the straight line suggests, and
	// letting that go unchecked is how you end up anchored across a level.
	const float flLength = barnacle_wrap.GetBool()
						 ? GetTonguePathLength( vecEye )
						 : ( vecAttach - vecEye ).Length();

	if ( flLength > sk_barnacle_range.GetFloat() * 1.25f )
		return false;

	// The tongue is an organic rope - if solid geometry gets between the
	// player and the attach point, it snaps.
	//
	// ...which is exactly what must NOT happen once the tongue can wrap.
	// Rounding a corner is the moment wrapping starts, and it is also the
	// moment this check fires, so it is off by default now. Set
	// sk_barnacle_break_on_los 1 to get the original behaviour back.
	if ( !sk_barnacle_break_on_los.GetBool() )
		return true;

	trace_t tr;
	CBaseEntity *pIgnore = m_bAttachedToEntity ? m_hAttachEntity.Get() : NULL;
	UTIL_TraceLine( vecEye, vecAttach, MASK_SOLID, pOwner, COLLISION_GROUP_NONE, &tr );

	if ( tr.fraction < 1.0f )
	{
		// Allow the trace to end on the attach entity itself, or (for world
		// attachments) very close to the stored point.
		if ( pIgnore && tr.m_pEnt == pIgnore )
			return true;
		if ( ( tr.endpos - vecAttach ).Length() < 16.0f )
			return true;
		return false;
	}

	return true;
}

//-----------------------------------------------------------------------------
// ATTACHED: latched but not pulling. Holding secondary starts the pull;
// primary cancels.
//-----------------------------------------------------------------------------
void CWeaponBarnacle::State_Attached( CBasePlayer *pOwner )
{
	if ( !IsAttachStillValid( pOwner ) )
	{
		Detach( true );
		return;
	}

	if ( pOwner->m_afButtonPressed & IN_ATTACK )
	{
		Detach( true );
		return;
	}

	if ( pOwner->m_nButtons & IN_ATTACK2 )
	{
		SetState( BARNACLE_PULLING );
		return;
	}

	// Keep wrapping up to date even when not pulling - you can walk around a
	// corner while just hanging there, and the path has to follow.
	if ( barnacle_wrap.GetBool() )
		UpdatePivots( pOwner, pOwner->EyePosition() );

	Vector vecAttach;
	GetAttachWorldPos( vecAttach );
	UpdateTongueVisual( pOwner, vecAttach );
}

//-----------------------------------------------------------------------------
// Where the pull actually aims: the nearest bend, not the anchor.
//
// That single change is what turns wrapping from decoration into a mechanic -
// pulled towards the corner you are behind, you swing around it and only then
// start heading for the anchor, instead of grinding face-first into the wall
// between you and it.
//
// Note what this function does NOT do: it does not decide when a bend is
// spent. UpdatePivots() owns that, and its criterion is line of sight, not
// distance. An earlier version popped bends here once the player got within
// sk_barnacle_corner_dist, which fought the solver directly - the pull removed
// a bend that still had tension on it, the solver put it straight back on the
// next trace, and the target flipped between the corner and the anchor every
// frame. One owner, one criterion.
//-----------------------------------------------------------------------------
Vector CWeaponBarnacle::GetPullTarget( CBasePlayer *pOwner ) const
{
	Vector vecAttach;
	GetAttachWorldPos( vecAttach );

	if ( !barnacle_wrap.GetBool() || m_Pivots.Count() <= 1 )
		return vecAttach;

	const int iLast = m_Pivots.Count() - 1;
	const BarnaclePivot_t &bend = m_Pivots[ iLast ];
	const Vector vecNext = m_Pivots[ iLast - 1 ].vecPos;	// node beyond the bend

	// Aim PAST the bend, never at it.
	//
	// A bend sits BARNACLE_PIVOT_OFFSET (2 units) off the surface it wraps.
	// The player hull is 32 units wide, so its centre can never get closer
	// than ~16 units to that surface - the bend is a point the player is
	// physically unable to reach. Steering straight at it drives the hull into
	// the wall beside the corner, the eye never rounds it, line of sight to
	// the next node never opens, the bend never unwraps, and the pull grinds
	// there forever.
	//
	// So the target is offset twice: along the next segment, so the player is
	// already being steered around the corner as they arrive, and away from
	// the surface, so the target is somewhere a player-sized hull can be.
	Vector vecLead = vecNext - bend.vecPos;
	const float flSegLen = VectorNormalize( vecLead );

	const float flLead = MIN( sk_barnacle_corner_lead.GetFloat(), flSegLen * 0.5f );

	return bend.vecPos
		 + vecLead * flLead
		 + bend.vecNormal * sk_barnacle_corner_clear.GetFloat();
}

//-----------------------------------------------------------------------------
// REELING: the anchor is prey, so the tongue winches it in.
//
// Mirrors State_Pulling deliberately - same clamped velocity steering, same
// stall guard, same path-aware target - with the roles swapped. Reeling along
// the bends rather than straight at the player means a crate dragged out of a
// side passage rounds the corner instead of grinding against its edge, for the
// same reason the player does.
//-----------------------------------------------------------------------------
void CWeaponBarnacle::State_Reeling( CBasePlayer *pOwner, float dt )
{
	CBaseEntity *pTarget = m_hAttachEntity.Get();

	if ( !pTarget )
	{
		Detach( false );	// already destroyed by something else
		return;
	}

	IPhysicsObject *pPhys = pTarget->VPhysicsGetObject();

	if ( !pPhys || !pPhys->IsMoveable() )
	{
		Detach( true );		// froze or turned into debris mid-pull
		return;
	}

	const bool bWrapped = ( m_Pivots.Count() > 1 );

	const Vector vecDest = bWrapped ? m_Pivots.Tail().vecPos
									: pOwner->WorldSpaceCenter();

	const Vector vecObject = pTarget->WorldSpaceCenter();

	Vector vecToDest = vecDest - vecObject;
	const float flDist = VectorNormalize( vecToDest );

	// Arrived at the mouth: bite. Only when the tongue runs straight to us -
	// while it is still wrapped, the object has only reached a bend.
	if ( !bWrapped && flDist <= sk_barnacle_crush_dist.GetFloat() )
	{
		CrushTarget( pOwner, pTarget );
		Detach( false );
		return;
	}

	// Stall: wedged in a doorway, or something heavy is sitting on it.
	if ( flDist < m_flClosestDist - 1.0f )
	{
		m_flClosestDist      = flDist;
		m_flLastProgressTime = gpGlobals->curtime;
	}
	else if ( gpGlobals->curtime - m_flLastProgressTime > sk_barnacle_stall_time.GetFloat() )
	{
		Detach( true );
		return;
	}

	// Impulse, NOT SetVelocity().
	//
	// Overwriting an object's velocity every tick fights the solver for
	// control of it. When the solver pushes the object out of a surface it
	// has penetrated, the next tick writes the old velocity straight back and
	// drives it in again; at reel speed the object eventually squeezes
	// through world geometry, and it spends the whole pull in a physics state
	// nothing else expects. Falling through floors and ending up non-solid
	// are both that.
	//
	// An impulse leaves collision resolution where it belongs. The cost is
	// that the reel is mass-dependent, so the impulse is scaled by mass to
	// keep a crate and a barrel coming in at the same rate.
	Vector vecVel;
	AngularImpulse angVel;
	pPhys->GetVelocity( &vecVel, &angVel );

	// Only the component along the pull matters; sideways motion is the
	// object swinging on the tongue, which is correct and left alone.
	const float flSpeedAlong = DotProduct( vecVel, vecToDest );
	const float flTargetSpeed = sk_barnacle_reel_speed.GetFloat();

	if ( flSpeedAlong < flTargetSpeed )
	{
		const float flDeltaV = MIN( flTargetSpeed - flSpeedAlong,
									sk_barnacle_reel_accel.GetFloat() * dt );

		pPhys->Wake();
		pPhys->ApplyForceCenter( vecToDest * ( flDeltaV * pPhys->GetMass() ) );
	}

	if ( sk_barnacle_debug.GetBool() )
		NDebugOverlay::Cross3D( vecObject, 8.0f, 255, 160, 0, true, 0.05f );

	Vector vecAttach;
	GetAttachWorldPos( vecAttach );
	UpdateTongueVisual( pOwner, vecAttach );
}

//-----------------------------------------------------------------------------
// The bite. Damage rather than outright removal, so the prop breaks the way
// its own prop_data says it should - gibs, sounds, spawned contents and all.
//
// A prop with no health in its model data will survive this. That is the
// model's business, not the weapon's: give it health in Hammer or pick a
// breakable model.
//-----------------------------------------------------------------------------
void CWeaponBarnacle::CrushTarget( CBasePlayer *pOwner, CBaseEntity *pTarget )
{
	if ( !pTarget )
		return;

	// Nothing touches the target's physics object here.
	//
	// An earlier revision damped its velocity first, reasoning that debris
	// inherits it. Whatever that bought, writing to the physics object of an
	// entity that is about to be destroyed in the same call is a good way to
	// leave a half-dead prop behind. If debris ends up flying past the camera,
	// raise sk_barnacle_crush_dist instead - that is a tuning problem, not a
	// reason to reach into vphysics.

	// Break, then remove. Both halves, because in this build neither path does
	// the whole job on its own.
	//
	// Verified from the console with no weapon involved:
	//
	//   ent_fire prop_physics Break  ->  debris spawns, the prop stays behind
	//                                    as a non-solid ghost, and firing it
	//                                    again spawns debris all over again
	//   damage                       ->  the prop is removed, no debris
	//
	// So the Break input runs the model's debris logic but never removes the
	// prop, and the damage path removes it without running that logic. Firing
	// the input and then removing the prop composes the two halves that work.
	//
	// This is papering over something. The real divergence lives in
	// src/game/server/props.cpp, between CBreakableProp::InputBreak() /
	// Break() and CPhysicsProp::Event_Killed() - one has lost its removal, the
	// other its call into the break logic. Worth fixing there: until it is,
	// every system in the mod that breaks a prop needs this same workaround,
	// which is the usual sign the fix belongs upstream rather than here.
	//
	// AcceptInput returns false for anything with no Break input, so this
	// self-selects: breakable props take this path, everything else falls
	// through to the damage below without a special case.
	if ( sk_barnacle_crush_input.GetBool() )
	{
		variant_t emptyVariant;

		if ( pTarget->AcceptInput( "Break", pOwner, this, emptyVariant, 0 ) )
		{
			// UTIL_Remove defers to the end of the frame, so the debris the
			// input just created is already spawned and survives this.
			UTIL_Remove( pTarget );

			if ( sk_barnacle_debug.GetBool() )
				DevMsg( "weapon_barnacle: bit '%s' via Break input, removed\n",
						pTarget->GetClassname() );

			return;
		}
	}

	// Scatter the pieces away from the player rather than through them, so
	// what does move ends up in view.
	Vector vecDir = pTarget->WorldSpaceCenter() - pOwner->EyePosition();

	if ( VectorNormalize( vecDir ) < 0.1f )
		vecDir = Vector( 0.0f, 0.0f, 1.0f );

	// Exactly lethal by default, rather than a fixed large number.
	//
	// The test crate reports 80. Hitting it for 200 is overkill, and
	// breakable debris carries health of its own - enough surplus damage can
	// take the pieces out in the same breath that created them, which reads in
	// game as the prop vanishing with only a noise to show for it. Asking the
	// target how much it needs sidesteps the question entirely.
	float flDamage = sk_barnacle_crush_damage.GetFloat();

	if ( flDamage < 0.0f )
		flDamage = ( pTarget->GetHealth() > 0 ) ? ( pTarget->GetHealth() + 1.0f ) : 100.0f;

	// DMG_CRUSH is what physics uses when it squashes something, and props can
	// treat it specially. A bite is closer to a melee hit, so DMG_CLUB is the
	// default - the same type the crowbar uses, which does produce debris.
	int nDamageType = sk_barnacle_crush_dmgtype.GetInt();

	if ( nDamageType == 0 )
		nDamageType = DMG_CLUB;

	// Inflictor is the player, not the weapon, and the damage position is the
	// point the tongue actually gripped rather than the middle of the prop.
	// Both match what a melee hit looks like; a damage position buried inside
	// the object is degenerate for anything that seeds debris from it.
	Vector vecAttach;
	GetAttachWorldPos( vecAttach );

	CTakeDamageInfo info( pOwner, pOwner, flDamage, nDamageType );
	info.SetDamagePosition( vecAttach );
	info.SetDamageForce( vecDir * sk_barnacle_crush_force.GetFloat() );

	pTarget->TakeDamage( info );

	if ( sk_barnacle_debug.GetBool() )
		DevMsg( "weapon_barnacle: bit '%s' (health %d) for %.0f, type %d\n",
				pTarget->GetClassname(), pTarget->GetHealth(), flDamage, nDamageType );
}

//-----------------------------------------------------------------------------
// PULLING: velocity steering with clamped acceleration.
//
// Every frame: desired = normalize(attach - player) * pull_speed, then move
// the player's actual velocity towards `desired` by at most pull_accel * dt.
//
// Mapping to the assignment's movement-quality requirements:
//   * no extreme acceleration - convergence rate hard-capped by pull_accel
//   * no slingshotting        - speed can never exceed pull_speed (the
//                               steering target itself is capped) and is
//                               clamped again on release
//   * natural inertia         - we blend FROM the player's existing velocity
//                               instead of hard-setting it; on release the
//                               player keeps their (capped) velocity
//   * engine-friendly         - only velocity is written; collision and
//                               sliding are still resolved by standard game
//                               movement, so the player can't be pushed
//                               through geometry
//-----------------------------------------------------------------------------
void CWeaponBarnacle::State_Pulling( CBasePlayer *pOwner, float dt )
{
	if ( !IsAttachStillValid( pOwner ) )
	{
		Detach( true );
		return;
	}

	// Manual cancel with primary.
	if ( pOwner->m_afButtonPressed & IN_ATTACK )
	{
		Detach( true );
		return;
	}

	// Releasing secondary stops the pull but keeps us attached.
	// Momentum handling: cap whatever speed we built up.
	if ( !( pOwner->m_nButtons & IN_ATTACK2 ) )
	{
		Vector vel = pOwner->GetAbsVelocity();
		const float flMax = sk_barnacle_release_speed.GetFloat();
		if ( vel.Length() > flMax )
		{
			VectorNormalize( vel );
			pOwner->SetAbsVelocity( vel * flMax );
		}
		SetState( BARNACLE_ATTACHED );
		return;
	}

	// Re-solve the wrap BEFORE deciding where to pull. Rounding a corner frees
	// the bend that was holding the tongue, and the pull has to notice in this
	// frame - keep aiming at a spent bend for even one more frame and the
	// player gets a visible tug towards a corner that is no longer there.
	//
	// The free end is the eye, the same reference the solver's line-of-sight
	// test uses. Steering from WorldSpaceCenter while unwrapping from the eye
	// would mean the two disagree by the better part of a player's height,
	// and the bend would be released well before or after the pull reacted.
	const Vector vecEye = pOwner->EyePosition();

	if ( barnacle_wrap.GetBool() )
		UpdatePivots( pOwner, vecEye );

	// Prey gets dragged to us instead of us to it. Everything above this line
	// - validity, cancel, release, wrapping - applies either way.
	if ( m_bReelTarget )
	{
		State_Reeling( pOwner, dt );
		return;
	}

	const Vector vecTarget = GetPullTarget( pOwner );

	Vector vecAttach;
	GetAttachWorldPos( vecAttach );

	Vector vecToTarget = vecTarget - vecEye;
	const float flDist = vecToTarget.Length();

	// Arrival is still measured from the player's centre so the existing
	// sk_barnacle_detach_dist tuning keeps its meaning.
	const float flAnchorDist = ( vecAttach - pOwner->WorldSpaceCenter() ).Length();

	// Arrival: auto-detach and damp velocity hard. This is what prevents
	// the "hang under the ceiling and pogo upwards forever" exploit -
	// you arrive, you stop, gravity takes over. Re-fire is additionally
	// gated by BARNACLE_REFIRE_DELAY.
	//
	// Only the anchor counts as arrival. GetPullTarget() guarantees that when
	// bends remain, vecTarget is one of them and not the anchor.
	const bool bAtAnchor = ( m_Pivots.Count() <= 1 );

	if ( bAtAnchor && flAnchorDist <= sk_barnacle_detach_dist.GetFloat() )
	{
		pOwner->SetAbsVelocity( pOwner->GetAbsVelocity() * sk_barnacle_arrive_damp.GetFloat() );
		Detach( false );
		return;
	}

	// Stall detection: latched but wedged against geometry with no progress
	// -> let go instead of grinding the player into a wall forever.
	//
	// Progress is measured along the whole path, not against the current
	// target. Otherwise every popped bend resets the metric and a genuinely
	// stuck player never trips the timer.
	const float flProgressMetric = barnacle_wrap.GetBool()
								 ? GetTonguePathLength( vecEye )
								 : flDist;

	if ( flProgressMetric < m_flClosestDist - 1.0f )
	{
		m_flClosestDist      = flProgressMetric;
		m_flLastProgressTime = gpGlobals->curtime;
	}
	else if ( gpGlobals->curtime - m_flLastProgressTime > sk_barnacle_stall_time.GetFloat() )
	{
		Detach( true );
		return;
	}

	// --- Velocity steering ---
	VectorNormalize( vecToTarget );
	const Vector vecDesiredVel = vecToTarget * sk_barnacle_pull_speed.GetFloat();

	Vector vel   = pOwner->GetAbsVelocity();
	Vector delta = vecDesiredVel - vel;

	const float flMaxDelta = sk_barnacle_pull_accel.GetFloat() * dt;
	if ( delta.Length() > flMaxDelta )
	{
		VectorNormalize( delta );
		delta *= flMaxDelta;
	}

	vel += delta;
	pOwner->SetAbsVelocity( vel );

	// Rope-borne: don't let ground friction eat the pull when starting
	// from the floor.
	pOwner->SetGroundEntity( NULL );

	if ( sk_barnacle_debug.GetBool() )
	{
		NDebugOverlay::Cross3D( vecAttach, 8.0f, 64, 255, 64, true, 0.05f );
		NDebugOverlay::Cross3D( vecTarget, 6.0f, 255, 160, 0, true, 0.05f );

		Vector vecPrev = vecEye;
		for ( int i = m_Pivots.Count() - 1; i >= 0; --i )
		{
			NDebugOverlay::Line( vecPrev, m_Pivots[i].vecPos, 255, 160, 0, true, 0.05f );
			vecPrev = m_Pivots[i].vecPos;
		}
	}

	UpdateTongueVisual( pOwner, vecAttach );
}

//-----------------------------------------------------------------------------
// Detach / retract: the single exit path back to IDLE from every state.
//-----------------------------------------------------------------------------
void CWeaponBarnacle::Detach( bool bKeepMomentum )
{
	CBasePlayer *pOwner = ToBasePlayer( GetOwner() );

	if ( pOwner && bKeepMomentum && m_iState == BARNACLE_PULLING )
	{
		// Momentum handling on letting go mid-pull: keep direction,
		// clamp magnitude (anti-slingshot).
		Vector vel = pOwner->GetAbsVelocity();
		const float flMax = sk_barnacle_release_speed.GetFloat();
		if ( vel.Length() > flMax )
		{
			VectorNormalize( vel );
			pOwner->SetAbsVelocity( vel * flMax );
		}
	}

	DestroyTongueVisual();

	m_Pivots.RemoveAll();

	m_bAttachedToEntity = false;
	m_bReelTarget       = false;
	m_hAttachEntity     = NULL;

	m_flNextPrimaryAttack = gpGlobals->curtime + BARNACLE_REFIRE_DELAY;

	SetState( BARNACLE_IDLE );
}

//-----------------------------------------------------------------------------
// Tongue visual: a beam from the muzzle to the tip / attach point.
// Explicitly a placeholder per the assignment - swappable for a rope or an
// animated model later without touching the state machine.
//-----------------------------------------------------------------------------
void CWeaponBarnacle::UpdateTongueVisual( CBasePlayer *pOwner, const Vector &vecTip )
{
	// The client owns the tongue visual now: it starts at the muzzle, wraps
	// corners and is drawn as a curve. This straight eye-to-tip beam would sit
	// right on top of it, so it is off unless explicitly asked for.
	if ( !sk_barnacle_server_beam.GetBool() )
	{
		DestroyTongueVisual();
		return;
	}

	const Vector vecStart = pOwner->Weapon_ShootPosition();

	CBeam *pBeam = m_hTongueBeam.Get();
	if ( !pBeam )
	{
		pBeam = CBeam::BeamCreate( BARNACLE_BEAM_SPRITE, 1.5f );
		if ( !pBeam )
			return;

		pBeam->PointsInit( vecStart, vecTip );
		pBeam->SetColor( 200, 90, 110 );	// fleshy pink - it *is* a tongue
		pBeam->SetBrightness( 220 );
		pBeam->SetNoise( 0.5f );
		pBeam->SetWidth( 1.5f );
		pBeam->SetEndWidth( 1.5f );
		m_hTongueBeam = pBeam;
	}

	pBeam->SetStartPos( vecStart );
	pBeam->SetEndPos( vecTip );

	if ( sk_barnacle_debug.GetBool() )
	{
		NDebugOverlay::Line( vecStart, vecTip, 255, 255, 0, true, 0.05f );
	}
}

void CWeaponBarnacle::DestroyTongueVisual( void )
{
	CBeam *pBeam = m_hTongueBeam.Get();
	if ( pBeam )
	{
		UTIL_Remove( pBeam );
		m_hTongueBeam = NULL;
	}
}

#endif // !CLIENT_DLL

#ifdef CLIENT_DLL
//=============================================================================
//
//                        CLIENT-SIDE TONGUE
//
// CLIENT ARCHITECTURE NOTE
//
// Everything below runs in client.dll only. The server code above is
// untouched: it still owns firing, attaching and pulling, and it still
// networks m_iState.
//
// The client does NOT receive the attach point. It does not need to: on the
// frame m_iState leaves IDLE, the client runs the same trace the server ran,
// from the same origin (eye position) with the same mask, against the same
// BSP. On world geometry the result is identical, so the tongue lands where
// the server put it with zero extra networking. If the two ever disagree
// (only possible on moving entities), the client re-syncs on the next state
// change - a purely cosmetic, sub-second discrepancy.
//
// What the client gains by owning the visual:
//   * the muzzle position is sampled from the view model bones that are
//     actually being rendered this frame - no lag, no jitter against the gun
//   * the wrap path updates at frame rate, not tick rate
//   * the curve can be re-tessellated per frame for free
//
//=============================================================================

//-----------------------------------------------------------------------------
// Small helper: normalized copy. (Vector::Normalized() is not available in
// every SDK 2013 revision, so we do it by hand.)
//-----------------------------------------------------------------------------
static inline Vector BarnacleNormalized( const Vector &v )
{
	Vector out = v;
	VectorNormalize( out );
	return out;
}

//-----------------------------------------------------------------------------
// Entity lifecycle
//-----------------------------------------------------------------------------
void CWeaponBarnacle::OnDataChanged( DataUpdateType_t updateType )
{
	BaseClass::OnDataChanged( updateType );

	if ( updateType == DATA_UPDATE_CREATED )
	{
		m_iPrevState    = BARNACLE_IDLE;
		m_bTongueActive = false;
		m_bLatched      = false;

		// The tongue has to be re-evaluated every rendered frame, not every
		// tick, or it will visibly lag the view model.
		SetNextClientThink( CLIENT_THINK_ALWAYS );
	}
}

void CWeaponBarnacle::UpdateOnRemove( void )
{
	TongueStop();
	BaseClass::UpdateOnRemove();
}

//-----------------------------------------------------------------------------
// Per-frame driver. Mirrors the server state machine by watching m_iState.
//-----------------------------------------------------------------------------
void CWeaponBarnacle::ClientThink( void )
{
	SetNextClientThink( CLIENT_THINK_ALWAYS );

	C_BasePlayer *pOwner = ToBasePlayer( GetOwner() );

	if ( !pOwner || m_iState == BARNACLE_IDLE )
	{
		if ( m_bTongueActive )
			TongueStop();

		m_iPrevState = m_iState;
		return;
	}

	// Not IDLE but nothing drawn yet: either the tongue was just fired, or we
	// arrived mid-state (late spawn, weapon switch, one network update that
	// skipped straight past FIRING). TongueStart handles both.
	if ( !m_bTongueActive )
		TongueStart( pOwner );

	m_iPrevState = m_iState;

	if ( m_bTongueActive )
		TongueUpdate( pOwner, gpGlobals->frametime );
}

//-----------------------------------------------------------------------------
// Launch: predict the landing point with a single trace, then animate the tip
// towards it. Cheaper and steadier than re-tracing the swept segment every
// frame, and it means the wrap solver has a real anchor from frame one.
//-----------------------------------------------------------------------------
void CWeaponBarnacle::TongueStart( C_BasePlayer *pOwner )
{
	m_Pivots.RemoveAll();
	m_RenderPath.RemoveAll();

	m_vecEyeStart = pOwner->EyePosition();

	Vector vecFwd;
	AngleVectors( pOwner->EyeAngles(), &vecFwd );
	m_vecFireDir = vecFwd;

	m_flTravel = 0.0f;
	m_bLatched = false;
	m_vecTip   = m_vecEyeStart;
	m_vecHand  = m_vecEyeStart;

	// Same origin, direction and mask as the server's swept trace.
	trace_t tr;
	UTIL_TraceLine( m_vecEyeStart,
					m_vecEyeStart + m_vecFireDir * sk_barnacle_range.GetFloat(),
					MASK_SOLID, pOwner, COLLISION_GROUP_NONE, &tr );

	m_bWillHit     = ( tr.fraction < 1.0f );
	m_vecHitPos    = tr.endpos;
	m_vecHitNormal = tr.plane.normal;
	m_hHitEnt      = tr.m_pEnt;
	m_flHitDist    = ( tr.endpos - m_vecEyeStart ).Length();

	m_bTongueActive = true;

	if ( !m_hRenderer.Get() )
		m_hRenderer = C_BarnacleTongueRenderer::Create( this, m_bWillHit ? m_vecHitPos : m_vecEyeStart );

	// Already attached when we got here: skip the extension animation.
	if ( m_iState == BARNACLE_ATTACHED || m_iState == BARNACLE_PULLING )
	{
		m_flTravel = m_flHitDist;
		m_vecTip   = m_vecHitPos;
		AddPivot( m_vecHitPos, m_vecHitNormal, m_hHitEnt.Get() );
		m_bLatched = true;
	}
}

void CWeaponBarnacle::TongueStop( void )
{
	m_bTongueActive = false;
	m_bLatched      = false;

	m_Pivots.RemoveAll();
	m_RenderPath.RemoveAll();

	C_BarnacleTongueRenderer *pRenderer = m_hRenderer.Get();
	if ( pRenderer )
	{
		pRenderer->Release();
		m_hRenderer = NULL;
	}
}

//-----------------------------------------------------------------------------
void CWeaponBarnacle::TongueUpdate( C_BasePlayer *pOwner, float dt )
{
	// TWO DIFFERENT POINTS, and the distinction matters more than it looks.
	//
	// m_vecHand is where the tongue is DRAWN from - down near the gun. It is
	// cosmetic and it is allowed to be anywhere, including a few units inside
	// a wall when you press against one.
	//
	// vecEye is what the SOLVER runs from, and it must be the eye, because
	// that is what the server uses. Feed the solver the gun position instead
	// and the two sides compute different paths from the same geometry: near a
	// corner the client accumulates bends the server never had and never
	// releases them. It is also the point that ends up inside geometry, which
	// makes every wrap trace start solid and wedges the solver completely.
	const Vector vecEye = pOwner->EyePosition();

	GetTongueOrigin( pOwner, m_vecHand );

	// Pivots sitting on doors / elevators / trains ride along with them.
	RefreshPivotWorldPositions();

	if ( !m_bLatched )
	{
		m_flTravel += sk_barnacle_tongue_speed.GetFloat() * dt;

		if ( m_bWillHit && m_flTravel >= m_flHitDist )
		{
			m_flTravel = m_flHitDist;
			m_vecTip   = m_vecHitPos;
			AddPivot( m_vecHitPos, m_vecHitNormal, m_hHitEnt.Get() );
			m_bLatched = true;
		}
		else
		{
			// The tip travels along the eye ray (that is what the server
			// traced); only the drawn start point is the muzzle.
			m_vecTip = m_vecEyeStart + m_vecFireDir * m_flTravel;
		}
	}

	if ( m_bLatched && barnacle_wrap.GetBool() )
		UpdatePivots( pOwner, vecEye );

	BuildRenderPath( m_RenderPath );

	// Keep the renderable sitting on the middle of the tongue.
	//
	// Two jobs in one assignment. The frustum test reads GetRenderBounds()
	// fresh every frame, so a correct origin plus real bounds stops the tongue
	// vanishing when you look away from the world origin. And because a
	// position change invalidates the entity's leaf registration, the same
	// call keeps that registration current - no ClientLeafSystem() poking, no
	// branch-specific API name to get wrong.
	C_BarnacleTongueRenderer *pRenderer = m_hRenderer.Get();

	if ( pRenderer )
	{
		Vector vecMins, vecMaxs;

		if ( GetTongueRenderBounds( vecMins, vecMaxs ) )
			pRenderer->SetAbsOrigin( ( vecMins + vecMaxs ) * 0.5f );
	}

	if ( cl_barnacle_debug.GetBool() )
	{
		for ( int i = 0; i < m_Pivots.Count(); ++i )
		{
			NDebugOverlay::Cross3D( m_Pivots[i].vecPos, 6.0f, 255, 220, 0, true, 0.0f );
			NDebugOverlay::Line( m_Pivots[i].vecPos,
								 m_Pivots[i].vecPos + m_Pivots[i].vecNormal * 12.0f,
								 0, 255, 255, true, 0.0f );
		}

		for ( int i = 0; i < m_RenderPath.Count() - 1; ++i )
			NDebugOverlay::Line( m_RenderPath[i], m_RenderPath[i + 1], 0, 255, 0, true, 0.0f );
	}
}

//-----------------------------------------------------------------------------
// Where the tongue visually leaves the weapon.
//
// The gameplay trace still starts at the eye - move that to the muzzle and
// shots stop matching the crosshair. Only the drawn start point moves.
//
// Default is a fixed offset in view space rather than a model attachment.
// That is not a fallback, it is the better default: view models render with
// their own FOV, so an attachment sampled in world space does not sit on the
// pixels you see anyway, and plenty of view models simply have no usable
// "muzzle" attachment. A tuned offset is predictable and costs nothing.
//-----------------------------------------------------------------------------
bool CWeaponBarnacle::GetTongueOrigin( CBasePlayer *pOwner, Vector &vecOut )
{
	QAngle angDummy;

	if ( cl_barnacle_origin_mode.GetInt() == 1 )
	{
		// First person: sample the view model that is actually on screen.
		if ( pOwner == C_BasePlayer::GetLocalPlayer() && !pOwner->ShouldDrawLocalPlayer() )
		{
			C_BaseViewModel *pVM = pOwner->GetViewModel();
			if ( pVM )
			{
				const int iAttachment = pVM->LookupAttachment( "muzzle" );
				if ( iAttachment > 0 && pVM->GetAttachment( iAttachment, vecOut, angDummy ) )
				{
#ifdef BARNACLE_CORRECT_VIEWMODEL_FOV
					// Converts a view model attachment into the world position
					// that lines up with what is rendered. Declared in view.h.
					FormatViewModelAttachment( vecOut, true );
#endif
					return true;
				}
			}
		}

		// Third person, or somebody else's barnacle: the world model's muzzle.
		const int iWorldAttachment = LookupAttachment( "muzzle" );
		if ( iWorldAttachment > 0 && GetAttachment( iWorldAttachment, vecOut, angDummy ) )
			return true;

		if ( cl_barnacle_debug.GetBool() )
			DevMsg( "weapon_barnacle: no usable 'muzzle' attachment, using view-space offset\n" );
	}

	// View-space offset: forward / right / down from the eye, tracking wherever
	// the player looks. Third person has no view model to speak of, so the
	// same offset off the eye is as good a guess as any.
	Vector vecFwd, vecRight, vecUp;
	AngleVectors( pOwner->EyeAngles(), &vecFwd, &vecRight, &vecUp );

	const Vector vecEye = pOwner->EyePosition();
	const Vector vecWanted = vecEye
						   + vecFwd   * cl_barnacle_origin_fwd.GetFloat()
						   + vecRight * cl_barnacle_origin_right.GetFloat()
						   + vecUp    * cl_barnacle_origin_up.GetFloat();

	// Pressed up against a wall, that offset lands inside it and the tongue
	// appears to sprout out of the brush. Sweep from the eye and stop short.
	trace_t tr;
	UTIL_TraceLine( vecEye, vecWanted, BARNACLE_WRAP_MASK, pOwner, COLLISION_GROUP_NONE, &tr );

	vecOut = ( tr.startsolid || tr.allsolid ) ? vecEye : tr.endpos;
	return true;
}

//=============================================================================
// RENDERING
//=============================================================================

//-----------------------------------------------------------------------------
// Path, muzzle first: [ hand, bend_n, ... bend_1, attach point ]
//-----------------------------------------------------------------------------
void CWeaponBarnacle::BuildRenderPath( CUtlVector< Vector > &path ) const
{
	path.RemoveAll();

	if ( !m_vecHand.IsValid() )
		return;

	path.AddToTail( m_vecHand );

	if ( m_bLatched )
	{
		for ( int i = m_Pivots.Count() - 1; i >= 0; --i )
		{
			// One bad vertex would blow up the render bounds and get the whole
			// tongue culled - which reads to the player as the line vanishing
			// for no reason. Drop the path instead of drawing garbage.
			if ( !m_Pivots[i].vecPos.IsValid() )
			{
				path.RemoveAll();
				return;
			}

			path.AddToTail( m_Pivots[i].vecPos );
		}
	}
	else
	{
		if ( !m_vecTip.IsValid() )
		{
			path.RemoveAll();
			return;
		}

		path.AddToTail( m_vecTip );
	}
}

//-----------------------------------------------------------------------------
// World-space bounds of the drawn path.
//-----------------------------------------------------------------------------
bool CWeaponBarnacle::GetTongueRenderBounds( Vector &mins, Vector &maxs ) const
{
	if ( m_RenderPath.Count() < 2 )
		return false;

	mins = maxs = m_RenderPath[0];

	for ( int i = 1; i < m_RenderPath.Count(); ++i )
	{
		VectorMin( mins, m_RenderPath[i], mins );
		VectorMax( maxs, m_RenderPath[i], maxs );
	}

	// Slack for the corner fillets, which bulge slightly outside the polyline,
	// and for the rendered thickness.
	const Vector vecPad( BARNACLE_RENDER_PAD, BARNACLE_RENDER_PAD, BARNACLE_RENDER_PAD );

	mins -= vecPad;
	maxs += vecPad;

	return true;
}

//-----------------------------------------------------------------------------
// Draw the tongue as a rounded polyline.
//
// The pivots are hard corners; the fillets are what make it read as a flexible
// tongue rather than bent wire. Each interior vertex becomes the control point
// of a quadratic Bezier that runs between points pulled `corner_round` units
// back along the two adjacent segments. Straight runs stay perfectly straight.
//-----------------------------------------------------------------------------
void CWeaponBarnacle::DrawTongue( void )
{
	const int nCount = m_RenderPath.Count();
	if ( nCount < 2 )
		return;

	// Debug path: no material, no beam code. Use this first to confirm the
	// wrapping logic is right before tuning how it looks.
	if ( !cl_barnacle_render.GetBool() )
	{
		for ( int i = 0; i < nCount - 1; ++i )
			NDebugOverlay::Line( m_RenderPath[i], m_RenderPath[i + 1], 255, 100, 130, true, 0.0f );
		return;
	}

	if ( !m_matTongue.IsValid() )
		m_matTongue.Init( BARNACLE_TONGUE_MATERIAL, TEXTURE_GROUP_CLIENT_EFFECTS );

	CMatRenderContextPtr pRenderContext( materials );
	pRenderContext->Bind( m_matTongue );

	const float  flWidth = cl_barnacle_width.GetFloat();
	const float  flRound = cl_barnacle_corner_round.GetFloat();
	const Vector vecColor( 0.78f, 0.35f, 0.43f );	// fleshy pink, 0..1 range

	for ( int i = 0; i < nCount - 1; ++i )
	{
		const Vector &a = m_RenderPath[i];
		const Vector &b = m_RenderPath[i + 1];

		const float flSegLen = ( b - a ).Length();
		if ( flSegLen < 0.1f )
			continue;

		// Never eat more than 45% of a segment per fillet, or short segments
		// between two close corners would invert.
		const float flTrim = MIN( flRound, flSegLen * 0.45f );

		Vector vecSegStart = a;
		Vector vecSegEnd   = b;

		if ( i > 0 )
			vecSegStart = a + BarnacleNormalized( b - a ) * flTrim;

		if ( i < nCount - 2 )
			vecSegEnd = b + BarnacleNormalized( a - b ) * flTrim;

		// Straight run. Control point on the midpoint = a straight line.
		DrawBeamQuadratic( vecSegStart,
						   ( vecSegStart + vecSegEnd ) * 0.5f,
						   vecSegEnd,
						   flWidth, vecColor, 0.0f );

		// Fillet through the corner at b.
		if ( i < nCount - 2 )
		{
			const Vector &c = m_RenderPath[i + 2];
			const float flNextLen = ( c - b ).Length();

			if ( flNextLen > 0.1f )
			{
				const Vector vecNextStart =
					b + BarnacleNormalized( c - b ) * MIN( flRound, flNextLen * 0.45f );

				DrawBeamQuadratic( vecSegEnd, b, vecNextStart, flWidth, vecColor, 0.0f );
			}
		}
	}
}

#endif // CLIENT_DLL
