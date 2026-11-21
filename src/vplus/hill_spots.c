/* King of the Hill's chosen spots (the game type's Hill spots: Chosen): 5-8 on each arena, one in each of its areas
 * (the route's sections: the bowls, rooftops, plazas and roads the game itself tells apart), the biggest first, spread
 * at least 2500 apart; picked from the arena's route points (the places cars drive, its PTS file) and checked in the
 * game (compare_builds' tourlook: each hill seen from 2400 away). By arena (the settings' +24: 1 Los Angeles .. 8
 * Hong Kong): x, y and z (the ground's) of each hill's middle. */
#include <stdint.h>

#define SPOTS_MAX 8
static const int32_t spots[9][SPOTS_MAX][3] = {
    {{0}},
    { /* 1 Los Angeles */
        {5360, 4304, 104},
        {-5280, 480, 240},
        {1952, -4368, 240},
        {0, 0, 0}, /* the middle of the interchange */
        {-5376, 5376, 0},
        {8064, 0, 0},
        {-7632, -4128, 160},
        {7632, -4128, 176},
    },
    { /* 2 Moscow */
        {-808, 808, 0}, /* under the dish's beam */
        {2960, 0, 600}, /* the stadium ring */
        {0, -2960, 600},
        {0, 2960, 600},
        {-2960, 0, 600},
        {640, -1440, 0}, /* the bowl, under the dish's rim */
    },
    { /* 3 Paris */
        {-208, 3360, 192},
        {0, -7920, 192}, /* under the Eiffel Tower */
        {3648, 5056, 768}, /* the raised walk */
        {0, -1824, 0},
        {1392, 1680, 768}, /* the statue's square */
        {-1400, 4800, 768},
        {-4800, 1536, 192},
        {4800, -288, 192},
    },
    { /* 4 Amazonia */
        {0, -5200, 200}, /* at the pyramid's foot */
        {0, 13200, 200},
        {-5920, 400, 200},
        {0, 5600, 200},
        {5200, 0, 200},
        {-5440, 9040, 120}, /* the sand */
        {-5600, -3080, 200},
        {5600, -3080, 200},
    },
    { /* 5 New York */
        {-4400, 3600, 7360},
        {6296, -3400, 6600},
        {-1200, -3400, 6400},
        {-5200, -2400, 8240},
        {6400, 0, 7400},
        {800, 2400, 7600},
        {280, -2400, 7360}, /* by the billboard */
        {-3000, 800, 7840},
    },
    { /* 6 Antarctica */
        {2424, 2712, 4800},
        {15256, -2736, 4000},
        {-200, -1040, 4800},
        {-2320, 5040, 4480}, /* the ice caves */
        {-2200, 1656, 4480},
        {-2280, -1656, 4480},
        {8672, 4896, 4800},
        {-11360, 1776, 4800},
    },
    { /* 7 Holland */
        {-560, 3920, 0},
        {3744, 3744, 0},
        {960, -5640, 0},
        {6280, -2400, 0},
        {1032, -760, 0},
        {3320, -3704, 0},
        {4240, 800, 0},
        {6280, 4000, 0}, /* inside the stone building */
    },
    { /* 8 Hong Kong */
        {0, -4800, 400}, /* the temple's statue */
        {11680, -880, -128}, /* the tunnel */
        {-11680, -3920, -128}, /* the tunnel */
        {0, 1064, 400},
        {5840, -10560, 400},
        {-2400, -10640, 400},
        {0, 0, 1776}, /* the high platform */
        {4440, -5960, 400},
    },
};
static const int spot_count[9] = {0, 8, 6, 8, 8, 8, 8, 8, 8};

int hill_spots(int arena, const int32_t (**xyz)[3])
{
    if (arena < 1 || arena > 8 || !spot_count[arena]) return 0;
    *xyz = spots[arena];
    return spot_count[arena];
}
