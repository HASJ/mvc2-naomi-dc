/* Ground bounce and wall bounce state, properties, and damage-reaction hooks. */
#ifndef PL_BOUNCE_H
#define PL_BOUNCE_H

typedef enum BOUNCE_PROP {
	BOUNCE_PROP_NONE = 0,
	BOUNCE_PROP_GROUND,
	BOUNCE_PROP_GROUND_FORCED,
	BOUNCE_PROP_WALL,
	BOUNCE_PROP_WALL_FORCED,
} BOUNCE_PROP;

extern void pl_bounce_reset_all( void );
extern void pl_bounce_combo_reset( const PLAYER_WORK *pl_w );
extern int pl_bounce_on_damage_pre( PLAYER_WORK *dm_w, const PLAYER_WORK *at_w, BOUNCE_PROP *prop_out, Uint8 *no_fly_screen_out );
extern void pl_bounce_on_damage_post( PLAYER_WORK *dm_w, const PLAYER_WORK *at_w, BOUNCE_PROP prop, Uint8 no_fly_screen );
extern int pl_bounce_dm_move( PLAYER_WORK *pl_w );

#endif
