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
#else
	#include "hl2mp_player.h"
	#include "beam_shared.h"
	#include "ndebugoverlay.h"
#endif

#include "weapon_hl2mpbasehlmpcombatweapon.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#ifndef CLIENT_DLL
//-----------------------------------------------------------------------------
// Tunables (server-side). Live-tunable so movement quality can be iterated
// on without recompiling. Defaults chosen to feel "controlled" (DESIGN.md).
//-----------------------------------------------------------------------------
static ConVar sk_barnacle_range        ( "sk_barnacle_range",         "1200", FCVAR_NONE, "Max tongue range in units. Beyond this the tongue retracts." );
static ConVar sk_barnacle_tongue_speed ( "sk_barnacle_tongue_speed",  "3500", FCVAR_NONE, "Speed at which the tongue tip extends (units/sec)." );
static ConVar sk_barnacle_pull_speed   ( "sk_barnacle_pull_speed",    "550",  FCVAR_NONE, "Target speed the player is pulled at (units/sec)." );
static ConVar sk_barnacle_pull_accel   ( "sk_barnacle_pull_accel",    "1600", FCVAR_NONE, "How fast player velocity converges to the pull vector (units/sec^2)." );
static ConVar sk_barnacle_detach_dist  ( "sk_barnacle_detach_dist",   "72",   FCVAR_NONE, "Distance from attach point at which we auto-detach (arrival)." );
static ConVar sk_barnacle_release_speed( "sk_barnacle_release_speed", "400",  FCVAR_NONE, "Max speed the player keeps when releasing the pull (anti-slingshot)." );
static ConVar sk_barnacle_arrive_damp  ( "sk_barnacle_arrive_damp",   "0.3",  FCVAR_NONE, "Velocity fraction kept on arrival auto-detach (prevents ceiling pogo)." );
static ConVar sk_barnacle_stall_time   ( "sk_barnacle_stall_time",    "1.0",  FCVAR_NONE, "If pulling makes no progress for this long, auto-detach." );
static ConVar sk_barnacle_debug        ( "sk_barnacle_debug",         "0",    FCVAR_NONE, "1 = draw debug overlays and log state transitions." );

#define BARNACLE_BEAM_SPRITE   "sprites/laserbeam.vmt"   // stock asset
#endif

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
	bool			GetAttachWorldPos( Vector &out ) const;
	bool			IsAttachStillValid( CBasePlayer *pOwner );
	void			UpdateTongueVisual( CBasePlayer *pOwner, const Vector &vecTip );
	void			DestroyTongueVisual( void );

	// --- Server-only data ----------------------------------------------
	Vector			m_vecTongueTip;			// current tip position while extending
	Vector			m_vecTongueDir;			// locked-in direction at fire time
	Vector			m_vecTongueStart;		// where the tongue was fired from

	Vector			m_vecAttachPoint;		// world-space attach point (static geometry)
	bool			m_bAttachedToEntity;
	EHANDLE			m_hAttachEntity;		// entity we latched onto (may move)
	Vector			m_vecAttachLocalOffset;	// attach point in entity-local space

	float			m_flClosestDist;		// best distance achieved this pull
	float			m_flLastProgressTime;	// last time we got measurably closer

	CHandle<CBeam>	m_hTongueBeam;
#endif // !CLIENT_DLL

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
	m_flClosestDist      = 0.0f;
	m_flLastProgressTime = 0.0f;
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

#ifndef CLIENT_DLL
//=============================================================================
// Everything below is server-only gameplay logic.
//=============================================================================

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
bool CWeaponBarnacle::IsSurfaceValid( const trace_t &tr ) const
{
	if ( tr.surface.flags & SURF_SKY )
		return false;

	if ( tr.m_pEnt && tr.m_pEnt == GetOwner() )
		return false;

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

	const Vector vecEye = pOwner->EyePosition();

	// Padded range check so we don't instantly break on the exact boundary.
	if ( ( vecAttach - vecEye ).Length() > sk_barnacle_range.GetFloat() * 1.25f )
		return false;

	// The tongue is an organic rope - if solid geometry gets between the
	// player and the attach point, it snaps.
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

	Vector vecAttach;
	GetAttachWorldPos( vecAttach );
	UpdateTongueVisual( pOwner, vecAttach );
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

	Vector vecAttach;
	GetAttachWorldPos( vecAttach );

	const Vector vecPlayer = pOwner->WorldSpaceCenter();
	Vector vecToAttach     = vecAttach - vecPlayer;
	const float flDist     = vecToAttach.Length();

	// Arrival: auto-detach and damp velocity hard. This is what prevents
	// the "hang under the ceiling and pogo upwards forever" exploit -
	// you arrive, you stop, gravity takes over. Re-fire is additionally
	// gated by BARNACLE_REFIRE_DELAY.
	if ( flDist <= sk_barnacle_detach_dist.GetFloat() )
	{
		pOwner->SetAbsVelocity( pOwner->GetAbsVelocity() * sk_barnacle_arrive_damp.GetFloat() );
		Detach( false );
		return;
	}

	// Stall detection: latched but wedged against geometry with no progress
	// -> let go instead of grinding the player into a wall forever.
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

	// --- Velocity steering ---
	VectorNormalize( vecToAttach );
	const Vector vecDesiredVel = vecToAttach * sk_barnacle_pull_speed.GetFloat();

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

	m_bAttachedToEntity = false;
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
