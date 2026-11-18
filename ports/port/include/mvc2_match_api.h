/*
 * mvc2_match_api.h  Canonical match control & introspection bridge between Dreamcast game module and host
 */

#ifndef MVC2_MATCH_API_H
#define MVC2_MATCH_API_H

typedef struct MVC2_MATCH_INFO {
    unsigned char ready;
    unsigned char in_select;
    unsigned char in_fight;
    unsigned char match_over;
    signed char winner_side;
    unsigned char stage_no;
    unsigned char vs_st;
    unsigned char routine_0;
    unsigned char routine_1;
    unsigned char p1_char[3];
    unsigned char p1_assist[3];
    unsigned char p1_color[3];
    unsigned char p2_char[3];
    unsigned char p2_assist[3];
    unsigned char p2_color[3];
} MVC2_MATCH_INFO;

typedef struct MVC2_MATCH_API {
    int (*is_ready)(void);
    void (*start_versus)(unsigned int seed);
    void (*start_training)(void);
    void (*get_info)(MVC2_MATCH_INFO* out);
} MVC2_MATCH_API;

#endif
