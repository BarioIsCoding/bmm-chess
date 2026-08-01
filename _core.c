/* Native core for bmm_chess. Square 0 = a1, 63 = h8; colour 1 = white. */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "_polyglot_keys.h"

typedef uint64_t U64;

#define PT_PAWN   1
#define PT_KNIGHT 2
#define PT_BISHOP 3
#define PT_ROOK   4
#define PT_QUEEN  5
#define PT_KING   6

#define CR_WK 1
#define CR_WQ 2
#define CR_BK 4
#define CR_BQ 8
#define CR_ALL 15

#define SQ_A1 0
#define SQ_C1 2
#define SQ_D1 3
#define SQ_E1 4
#define SQ_F1 5
#define SQ_G1 6
#define SQ_H1 7
#define SQ_A8 56
#define SQ_C8 58
#define SQ_D8 59
#define SQ_E8 60
#define SQ_F8 61
#define SQ_G8 62
#define SQ_H8 63

#define FILE_A_BB 0x0101010101010101ULL
#define FILE_H_BB 0x8080808080808080ULL
#define RANK_1_BB 0x00000000000000FFULL
#define RANK_8_BB 0xFF00000000000000ULL

#define BB(sq) (1ULL << (sq))

#define STARTING_FEN_C "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

/* bit utilities */

#if defined(_MSC_VER)
#include <intrin.h>
static __forceinline int ctz64(U64 v)
{
    unsigned long idx;
    _BitScanForward64(&idx, v);
    return (int)idx;
}
#elif defined(__GNUC__) || defined(__clang__)
static inline int ctz64(U64 v) { return __builtin_ctzll(v); }
#else
static int ctz64(U64 v)
{
    int n = 0;
    while (!(v & 1ULL)) { v >>= 1; n++; }
    return n;
}
#endif

/* No POPCNT instruction required, so this runs anywhere. */
static inline int popcount64(U64 x)
{
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return (int)((x * 0x0101010101010101ULL) >> 56);
}

static inline int pop_lsb(U64 *b)
{
    int s = ctz64(*b);
    *b &= *b - 1;
    return s;
}

#if defined(_MSC_VER)
static __forceinline int msb64(U64 v)
{
    unsigned long idx;
    _BitScanReverse64(&idx, v);
    return (int)idx;
}
#elif defined(__GNUC__) || defined(__clang__)
static inline int msb64(U64 v) { return 63 - __builtin_clzll(v); }
#else
static int msb64(U64 v)
{
    int n = 0;
    while (v >>= 1) n++;
    return n;
}
#endif

static U64 flip_vertical_bb(U64 b)
{
    return ((b >> 56) & 0x00000000000000FFULL) |
           ((b >> 40) & 0x000000000000FF00ULL) |
           ((b >> 24) & 0x0000000000FF0000ULL) |
           ((b >>  8) & 0x00000000FF000000ULL) |
           ((b <<  8) & 0x000000FF00000000ULL) |
           ((b << 24) & 0x0000FF0000000000ULL) |
           ((b << 40) & 0x00FF000000000000ULL) |
           ((b << 56) & 0xFF00000000000000ULL);
}

static U64 flip_horizontal_bb(U64 b)
{
    b = ((b >> 1) & 0x5555555555555555ULL) | ((b & 0x5555555555555555ULL) << 1);
    b = ((b >> 2) & 0x3333333333333333ULL) | ((b & 0x3333333333333333ULL) << 2);
    b = ((b >> 4) & 0x0F0F0F0F0F0F0F0FULL) | ((b & 0x0F0F0F0F0F0F0F0FULL) << 4);
    return b;
}

static U64 flip_diagonal_bb(U64 x)
{
    U64 t;
    t = 0x0F0F0F0F00000000ULL & (x ^ (x << 28)); x ^= t ^ (t >> 28);
    t = 0x3333000033330000ULL & (x ^ (x << 14)); x ^= t ^ (t >> 14);
    t = 0x5500550055005500ULL & (x ^ (x <<  7)); x ^= t ^ (t >>  7);
    return x;
}

static U64 flip_anti_diagonal_bb(U64 x)
{
    U64 t;
    t = x ^ (x << 36); x ^= 0xF0F0F0F00F0F0F0FULL & (t ^ (x >> 36));
    t = 0xCCCC0000CCCC0000ULL & (x ^ (x << 18)); x ^= t ^ (t >> 18);
    t = 0xAA00AA00AA00AA00ULL & (x ^ (x <<  9)); x ^= t ^ (t >>  9);
    return x;
}

static PyObject *InvalidMoveError = NULL;
static PyObject *IllegalMoveError = NULL;
static PyObject *AmbiguousMoveError = NULL;

/* attack tables */

static U64 KNIGHT_ATT[64];
static U64 KING_ATT[64];
static U64 PAWN_ATT[2][64];      /* [0] = black pawn on sq, [1] = white */
static U64 BETWEEN[64][64];      /* squares strictly between two aligned squares */
static U64 RAY[64][64];          /* the whole line through two aligned squares */
static U64 ROOK_RAYS[64];        /* rook attacks on an empty board */
static U64 BISHOP_RAYS[64];      /* bishop attacks on an empty board */

static const char FILE_CHARS[9] = "abcdefgh";
static const char RANK_CHARS[9] = "12345678";
static char SQ_NAMES[64][3];
static const char PIECE_CHARS[7] = " pnbrqk";

static const char *PIECE_NAMES_C[7] = {
    "", "pawn", "knight", "bishop", "rook", "queen", "king"
};

/* [colour][piece_type] -> UTF-8 glyph; colour 1 = white */
static const char *UNICODE_PIECES[2][7] = {
    { "?", "\xE2\x99\x9F", "\xE2\x99\x9E", "\xE2\x99\x9D",
      "\xE2\x99\x9C", "\xE2\x99\x9B", "\xE2\x99\x9A" },
    { "?", "\xE2\x99\x99", "\xE2\x99\x98", "\xE2\x99\x97",
      "\xE2\x99\x96", "\xE2\x99\x95", "\xE2\x99\x94" }
};

static U64 slider_attacks_slow(int is_rook, int sq, U64 occ)
{
    static const int RDIR[4][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    static const int BDIR[4][2] = { { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };
    const int (*dirs)[2] = is_rook ? RDIR : BDIR;
    U64 att = 0;
    int r0 = sq >> 3, f0 = sq & 7, d;

    for (d = 0; d < 4; d++) {
        int r = r0 + dirs[d][0], f = f0 + dirs[d][1];
        while (r >= 0 && r <= 7 && f >= 0 && f <= 7) {
            int t = r * 8 + f;
            att |= BB(t);
            if (occ & BB(t)) break;
            r += dirs[d][0];
            f += dirs[d][1];
        }
    }
    return att;
}

/* magic bitboards */

typedef struct {
    U64 mask;
    U64 magic;
    U64 *att;
    unsigned shift;
} Magic;

static Magic M_ROOK[64];
static Magic M_BISHOP[64];
static U64 ROOK_TABLE[102400];
static U64 BISHOP_TABLE[5248];

static U64 rng_state = 0x9E3779B97F4A7C15ULL;

static U64 rng64(void)
{
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 2685821657736338717ULL;
}

static U64 sparse_rand(void) { return rng64() & rng64() & rng64(); }

static void init_magics(int is_rook, Magic *m, U64 *table)
{
    U64 occupancy[4096], reference[4096];
    int epoch[4096];
    int cnt = 0, s;
    U64 *ptr = table;

    memset(epoch, 0, sizeof(epoch));

    for (s = 0; s < 64; s++) {
        U64 rank_bb = RANK_1_BB << (8 * (s >> 3));
        U64 file_bb = FILE_A_BB << (s & 7);
        U64 edges = ((RANK_1_BB | RANK_8_BB) & ~rank_bb) |
                    ((FILE_A_BB | FILE_H_BB) & ~file_bb);
        U64 b;
        int size = 0, i;

        m[s].mask = slider_attacks_slow(is_rook, s, 0) & ~edges;
        m[s].shift = 64u - (unsigned)popcount64(m[s].mask);
        m[s].att = ptr;

        b = 0;
        do {
            occupancy[size] = b;
            reference[size] = slider_attacks_slow(is_rook, s, b);
            size++;
            b = (b - m[s].mask) & m[s].mask;
        } while (b);
        ptr += size;

        for (;;) {
            int ok = 1;
            do {
                m[s].magic = sparse_rand();
            } while (popcount64((m[s].mask * m[s].magic) >> 56) < 6);
            cnt++;
            for (i = 0; i < size; i++) {
                unsigned idx = (unsigned)((occupancy[i] * m[s].magic) >> m[s].shift);
                if (epoch[idx] < cnt) {
                    epoch[idx] = cnt;
                    m[s].att[idx] = reference[i];
                } else if (m[s].att[idx] != reference[i]) {
                    ok = 0;
                    break;
                }
            }
            if (ok) break;
        }
    }
}

static inline U64 rook_attacks(int s, U64 occ)
{
    const Magic *m = &M_ROOK[s];
    return m->att[((occ & m->mask) * m->magic) >> m->shift];
}

static inline U64 bishop_attacks(int s, U64 occ)
{
    const Magic *m = &M_BISHOP[s];
    return m->att[((occ & m->mask) * m->magic) >> m->shift];
}

static inline U64 queen_attacks(int s, U64 occ)
{
    return rook_attacks(s, occ) | bishop_attacks(s, occ);
}

static void init_tables(void)
{
    static const int NDR[8] = { -2, -2, -1, -1, 1, 1, 2, 2 };
    static const int NDF[8] = { -1, 1, -2, 2, -2, 2, -1, 1 };
    int sq, i, a, b;

    for (sq = 0; sq < 64; sq++) {
        int r = sq >> 3, f = sq & 7;
        U64 kn = 0, kg = 0, wp = 0, bp = 0;

        for (i = 0; i < 8; i++) {
            int nr = r + NDR[i], nf = f + NDF[i];
            if (nr >= 0 && nr <= 7 && nf >= 0 && nf <= 7)
                kn |= BB(nr * 8 + nf);
        }
        for (i = 0; i < 9; i++) {
            int nr = r + (i / 3) - 1, nf = f + (i % 3) - 1;
            if (i == 4) continue;
            if (nr >= 0 && nr <= 7 && nf >= 0 && nf <= 7)
                kg |= BB(nr * 8 + nf);
        }
        if (r < 7) {
            if (f > 0) wp |= BB(sq + 7);
            if (f < 7) wp |= BB(sq + 9);
        }
        if (r > 0) {
            if (f > 0) bp |= BB(sq - 9);
            if (f < 7) bp |= BB(sq - 7);
        }

        KNIGHT_ATT[sq] = kn;
        KING_ATT[sq] = kg;
        PAWN_ATT[1][sq] = wp;
        PAWN_ATT[0][sq] = bp;

        ROOK_RAYS[sq] = slider_attacks_slow(1, sq, 0);
        BISHOP_RAYS[sq] = slider_attacks_slow(0, sq, 0);

        SQ_NAMES[sq][0] = FILE_CHARS[f];
        SQ_NAMES[sq][1] = RANK_CHARS[r];
        SQ_NAMES[sq][2] = '\0';
    }

    for (a = 0; a < 64; a++) {
        int ra = a >> 3, fa = a & 7;
        for (b = 0; b < 64; b++) {
            int rb = b >> 3, fb = b & 7;
            int dr, df, r, f;
            U64 bb = 0;

            BETWEEN[a][b] = 0;
            RAY[a][b] = 0;
            if (a == b) continue;
            if (!(ra == rb || fa == fb ||
                  (ra - rb == fa - fb) || (ra - rb == fb - fa)))
                continue;

            dr = (rb > ra) - (rb < ra);
            df = (fb > fa) - (fb < fa);
            r = ra + dr;
            f = fa + df;
            while (r != rb || f != fb) {
                bb |= BB(r * 8 + f);
                r += dr;
                f += df;
            }
            BETWEEN[a][b] = bb;

            /* The full line, extended past both squares to the board edge. */
            {
                U64 line = BB(a) | BB(b);
                int rr = ra, ff = fa;
                while (rr - dr >= 0 && rr - dr <= 7 && ff - df >= 0 && ff - df <= 7) {
                    rr -= dr; ff -= df;
                    line |= BB(rr * 8 + ff);
                }
                rr = rb; ff = fb;
                while (rr + dr >= 0 && rr + dr <= 7 && ff + df >= 0 && ff + df <= 7) {
                    rr += dr; ff += df;
                    line |= BB(rr * 8 + ff);
                }
                RAY[a][b] = line | bb;
            }
        }
    }

    init_magics(1, M_ROOK, ROOK_TABLE);
    init_magics(0, M_BISHOP, BISHOP_TABLE);
}

/* position */

typedef struct {
    U64 pawns, knights, bishops, rooks, queens, kings;
    U64 white, black;
    int turn;               /* 1 white, 0 black */
    int castling;
    int ep;                 /* -1 when there is none */
    int halfmove;
    int fullmove;
} Pos;

typedef struct {
    Pos pos;
    int from, to, promo;
} Undo;

static inline U64 pos_occupied(const Pos *p) { return p->white | p->black; }

static inline int piece_type_at(const Pos *p, int sq)
{
    U64 m = BB(sq);
    if (!((p->white | p->black) & m)) return 0;
    if (p->pawns & m) return PT_PAWN;
    if (p->knights & m) return PT_KNIGHT;
    if (p->bishops & m) return PT_BISHOP;
    if (p->rooks & m) return PT_ROOK;
    if (p->queens & m) return PT_QUEEN;
    if (p->kings & m) return PT_KING;
    return 0;
}

static inline void clear_square(Pos *p, int sq)
{
    U64 inv = ~BB(sq);
    p->pawns &= inv;
    p->knights &= inv;
    p->bishops &= inv;
    p->rooks &= inv;
    p->queens &= inv;
    p->kings &= inv;
    p->white &= inv;
    p->black &= inv;
}

static inline void set_piece(Pos *p, int sq, int pt, int color)
{
    U64 m = BB(sq);
    clear_square(p, sq);
    switch (pt) {
        case PT_PAWN:   p->pawns |= m;   break;
        case PT_KNIGHT: p->knights |= m; break;
        case PT_BISHOP: p->bishops |= m; break;
        case PT_ROOK:   p->rooks |= m;   break;
        case PT_QUEEN:  p->queens |= m;  break;
        case PT_KING:   p->kings |= m;   break;
        default: break;
    }
    if (color) p->white |= m;
    else p->black |= m;
}

static inline int king_square(const Pos *p, int color)
{
    U64 k = p->kings & (color ? p->white : p->black);
    if (!k) return -1;
    return ctz64(k);
}

/* Attackers of `color` on `sq`, for an explicit occupancy. */
static inline U64 attackers_to(const Pos *p, int color, int sq, U64 occ)
{
    U64 q = p->queens;
    U64 att = (PAWN_ATT[color ? 0 : 1][sq] & p->pawns)
            | (KNIGHT_ATT[sq] & p->knights)
            | (KING_ATT[sq] & p->kings)
            | (rook_attacks(sq, occ) & (p->rooks | q))
            | (bishop_attacks(sq, occ) & (p->bishops | q));
    return att & (color ? p->white : p->black);
}

static inline int square_attacked(const Pos *p, int color, int sq, U64 occ)
{
    return attackers_to(p, color, sq, occ) != 0;
}

static U64 pinned_mask(const Pos *p, int king_sq)
{
    U64 our = p->turn ? p->white : p->black;
    U64 their = p->turn ? p->black : p->white;
    U64 occ = our | their;
    U64 q = p->queens;
    U64 snipers = ((ROOK_RAYS[king_sq] & (p->rooks | q)) |
                   (BISHOP_RAYS[king_sq] & (p->bishops | q))) & their;
    U64 pinned = 0;

    while (snipers) {
        int s = pop_lsb(&snipers);
        U64 blockers = BETWEEN[king_sq][s] & occ;
        if (blockers && !(blockers & (blockers - 1)))
            pinned |= blockers & our;
    }
    return pinned;
}

/* Is our king attacked after the move? */
static int king_attacked_after(const Pos *p, int from, int to, int promo,
                               int king_sq)
{
    int us = p->turn;
    U64 from_mask = BB(from), to_mask = BB(to), ep_mask = 0, clear;
    int pt = piece_type_at(p, from);
    int landed, target;
    Pos t;

    if (!pt) return 1;

    if (pt == PT_PAWN && p->ep >= 0 && to == p->ep && (from & 7) != (to & 7))
        ep_mask = BB(us ? to - 8 : to + 8);

    t = *p;
    clear = ~(from_mask | to_mask | ep_mask);
    t.pawns &= clear;
    t.knights &= clear;
    t.bishops &= clear;
    t.rooks &= clear;
    t.queens &= clear;
    t.kings &= clear;
    t.white &= clear;
    t.black &= clear;

    landed = promo ? promo : pt;
    switch (landed) {
        case PT_PAWN:   t.pawns |= to_mask;   break;
        case PT_KNIGHT: t.knights |= to_mask; break;
        case PT_BISHOP: t.bishops |= to_mask; break;
        case PT_ROOK:   t.rooks |= to_mask;   break;
        case PT_QUEEN:  t.queens |= to_mask;  break;
        case PT_KING:   t.kings |= to_mask;   break;
        default: break;
    }
    if (us) t.white |= to_mask;
    else t.black |= to_mask;

    target = (pt == PT_KING) ? to : king_sq;
    return square_attacked(&t, !us, target, t.white | t.black);
}

/* move lists */

/* A move packs into one int: from | to << 6 | promotion << 12. */
#define MV(from, to, promo) ((from) | ((to) << 6) | ((promo) << 12))
#define MV_FROM(m)  ((m) & 63)
#define MV_TO(m)    (((m) >> 6) & 63)
#define MV_PROMO(m) (((m) >> 12) & 7)

#define ML_INLINE 256

typedef struct {
    int *v;
    int n;
    int cap;
    int buf[ML_INLINE];
} MoveList;

static void ml_init(MoveList *ml)
{
    ml->v = ml->buf;
    ml->n = 0;
    ml->cap = ML_INLINE;
}

static void ml_free(MoveList *ml)
{
    if (ml->v != ml->buf) free(ml->v);
    ml->v = ml->buf;
    ml->n = 0;
    ml->cap = ML_INLINE;
}

/* Heap-grow rather than truncate: a crafted FEN can exceed the buffer. */
static int ml_grow(MoveList *ml)
{
    int newcap = ml->cap * 2;
    int *nv;

    if (ml->v == ml->buf) {
        nv = (int *)malloc(sizeof(int) * (size_t)newcap);
        if (!nv) return -1;
        memcpy(nv, ml->buf, sizeof(int) * (size_t)ml->n);
    } else {
        nv = (int *)realloc(ml->v, sizeof(int) * (size_t)newcap);
        if (!nv) return -1;
    }
    ml->v = nv;
    ml->cap = newcap;
    return 0;
}

static inline void ml_add(MoveList *ml, int from, int to, int promo)
{
    if (ml->n == ml->cap && ml_grow(ml) < 0) return;
    ml->v[ml->n++] = MV(from, to, promo);
}

/* castling */

/* [colour][kingside] */
static const int CASTLE_RIGHT[2][2] = { { CR_BQ, CR_BK }, { CR_WQ, CR_WK } };
static const int CASTLE_ROOK_FROM[2][2] = { { SQ_A8, SQ_H8 }, { SQ_A1, SQ_H1 } };
static const int CASTLE_ROOK_TO[2][2] = { { SQ_D8, SQ_F8 }, { SQ_D1, SQ_F1 } };
static const int CASTLE_KING_FROM[2] = { SQ_E8, SQ_E1 };
static const int CASTLE_KING_TO[2][2] = { { SQ_C8, SQ_G8 }, { SQ_C1, SQ_G1 } };
static const U64 CASTLE_EMPTY[2][2] = {
    { BB(57) | BB(SQ_C8) | BB(SQ_D8), BB(SQ_F8) | BB(SQ_G8) },   /* b8 c8 d8 */
    { BB(1)  | BB(SQ_C1) | BB(SQ_D1), BB(SQ_F1) | BB(SQ_G1) }    /* b1 c1 d1 */
};
static const int CASTLE_SAFE[2][2][3] = {
    { { SQ_C8, SQ_D8, SQ_E8 }, { SQ_E8, SQ_F8, SQ_G8 } },
    { { SQ_C1, SQ_D1, SQ_E1 }, { SQ_E1, SQ_F1, SQ_G1 } }
};

static int can_castle(const Pos *p, int color, int kingside)
{
    U64 ours, occ;
    int i;

    if (!(p->castling & CASTLE_RIGHT[color][kingside])) return 0;

    /* FEN rights are not self-validating. */
    ours = color ? p->white : p->black;
    if (!(p->kings & ours & BB(CASTLE_KING_FROM[color]))) return 0;
    if (!(p->rooks & ours & BB(CASTLE_ROOK_FROM[color][kingside]))) return 0;

    occ = p->white | p->black;
    if (occ & CASTLE_EMPTY[color][kingside]) return 0;

    for (i = 0; i < 3; i++)
        if (square_attacked(p, !color, CASTLE_SAFE[color][kingside][i], occ))
            return 0;

    return 1;
}

/* pseudo-legal generation */

static const int PROMO_ORDER[4] = { PT_QUEEN, PT_ROOK, PT_BISHOP, PT_KNIGHT };

static void gen_pawn_moves(const Pos *p, MoveList *ml)
{
    int white = p->turn;
    U64 our_pawns = p->pawns & (white ? p->white : p->black);
    U64 their = white ? p->black : p->white;
    U64 occ = p->white | p->black;
    U64 single, dbl, lcap, rcap, promo_rank, bb;
    int i, k;

    if (white) {
        single = (our_pawns << 8) & ~occ;
        dbl = ((single & 0x0000000000FF0000ULL) << 8) & ~occ;
        lcap = ((our_pawns & ~FILE_A_BB) << 7) & their;
        rcap = ((our_pawns & ~FILE_H_BB) << 9) & their;
        promo_rank = RANK_8_BB;
    } else {
        single = (our_pawns >> 8) & ~occ;
        dbl = ((single & 0x0000FF0000000000ULL) >> 8) & ~occ;
        lcap = ((our_pawns & ~FILE_A_BB) >> 9) & their;
        rcap = ((our_pawns & ~FILE_H_BB) >> 7) & their;
        promo_rank = RANK_1_BB;
    }

    bb = single;
    while (bb) {
        int to = pop_lsb(&bb);
        int from = white ? to - 8 : to + 8;
        if (BB(to) & promo_rank) {
            for (k = 0; k < 4; k++) ml_add(ml, from, to, PROMO_ORDER[k]);
        } else {
            ml_add(ml, from, to, 0);
        }
    }

    bb = dbl;
    while (bb) {
        int to = pop_lsb(&bb);
        ml_add(ml, white ? to - 16 : to + 16, to, 0);
    }

    for (i = 0; i < 2; i++) {
        int delta = i == 0 ? (white ? 7 : -9) : (white ? 9 : -7);
        bb = i == 0 ? lcap : rcap;
        while (bb) {
            int to = pop_lsb(&bb);
            int from = to - delta;
            if (BB(to) & promo_rank) {
                for (k = 0; k < 4; k++) ml_add(ml, from, to, PROMO_ORDER[k]);
            } else {
                ml_add(ml, from, to, 0);
            }
        }
    }

    if (p->ep >= 0) {
        U64 epm = BB(p->ep);
        U64 lep, rep;
        if (white) {
            lep = ((our_pawns & ~FILE_A_BB) << 7) & epm;
            rep = ((our_pawns & ~FILE_H_BB) << 9) & epm;
        } else {
            lep = ((our_pawns & ~FILE_A_BB) >> 9) & epm;
            rep = ((our_pawns & ~FILE_H_BB) >> 7) & epm;
        }
        if (lep) ml_add(ml, white ? p->ep - 7 : p->ep + 9, p->ep, 0);
        if (rep) ml_add(ml, white ? p->ep - 9 : p->ep + 7, p->ep, 0);
    }
}

static void gen_pseudo(const Pos *p, MoveList *ml)
{
    U64 our = p->turn ? p->white : p->black;
    U64 occ = p->white | p->black;
    U64 not_ours = ~our;
    U64 pieces, attacks, king_bb;

    gen_pawn_moves(p, ml);

    pieces = p->knights & our;
    while (pieces) {
        int from = pop_lsb(&pieces);
        attacks = KNIGHT_ATT[from] & not_ours;
        while (attacks) ml_add(ml, from, pop_lsb(&attacks), 0);
    }

    pieces = p->bishops & our;
    while (pieces) {
        int from = pop_lsb(&pieces);
        attacks = bishop_attacks(from, occ) & not_ours;
        while (attacks) ml_add(ml, from, pop_lsb(&attacks), 0);
    }

    pieces = p->rooks & our;
    while (pieces) {
        int from = pop_lsb(&pieces);
        attacks = rook_attacks(from, occ) & not_ours;
        while (attacks) ml_add(ml, from, pop_lsb(&attacks), 0);
    }

    pieces = p->queens & our;
    while (pieces) {
        int from = pop_lsb(&pieces);
        attacks = queen_attacks(from, occ) & not_ours;
        while (attacks) ml_add(ml, from, pop_lsb(&attacks), 0);
    }

    king_bb = p->kings & our;
    if (king_bb) {
        int from = ctz64(king_bb);
        attacks = KING_ATT[from] & not_ours;
        while (attacks) ml_add(ml, from, pop_lsb(&attacks), 0);
    }

    if (can_castle(p, p->turn, 1))
        ml_add(ml, CASTLE_KING_FROM[p->turn], CASTLE_KING_TO[p->turn][1], 0);
    if (can_castle(p, p->turn, 0))
        ml_add(ml, CASTLE_KING_FROM[p->turn], CASTLE_KING_TO[p->turn][0], 0);
}

/* legal generation */

/* Only king moves, pins, check and en passant need verifying. */
static void gen_legal(const Pos *p, MoveList *ml)
{
    MoveList pseudo;
    int king_sq = king_square(p, p->turn);
    int in_check, i;
    U64 pinned;

    if (king_sq < 0) return;

    ml_init(&pseudo);
    gen_pseudo(p, &pseudo);

    in_check = square_attacked(p, !p->turn, king_sq, pos_occupied(p));
    pinned = pinned_mask(p, king_sq);

    for (i = 0; i < pseudo.n; i++) {
        int m = pseudo.v[i];
        int from = MV_FROM(m), to = MV_TO(m);
        if (in_check || from == king_sq || (BB(from) & pinned) || to == p->ep) {
            if (king_attacked_after(p, from, to, MV_PROMO(m), king_sq))
                continue;
        }
        ml_add(ml, from, to, MV_PROMO(m));
    }
    ml_free(&pseudo);
}

/* Same filter, no allocation; stop_at_one short-circuits the mate tests. */
static int count_legal(const Pos *p, int stop_at_one)
{
    MoveList pseudo;
    int king_sq = king_square(p, p->turn);
    int in_check, i, n = 0;
    U64 pinned;

    if (king_sq < 0) return 0;

    ml_init(&pseudo);
    gen_pseudo(p, &pseudo);

    in_check = square_attacked(p, !p->turn, king_sq, pos_occupied(p));
    pinned = pinned_mask(p, king_sq);

    for (i = 0; i < pseudo.n; i++) {
        int m = pseudo.v[i];
        int from = MV_FROM(m), to = MV_TO(m);
        if (in_check || from == king_sq || (BB(from) & pinned) || to == p->ep) {
            if (king_attacked_after(p, from, to, MV_PROMO(m), king_sq))
                continue;
        }
        n++;
        if (stop_at_one) break;
    }
    ml_free(&pseudo);
    return n;
}

/* make move */

/* Returns -1 when the from-square is empty. */
static int do_move(Pos *p, int from, int to, int promo)
{
    int pt, color, captured, ep_captured = -1, final_pt;

    if (from == to && promo == 0) {
        p->ep = -1;
        p->halfmove += 1;
        if (!p->turn) p->fullmove += 1;
        p->turn = !p->turn;
        return 0;
    }

    pt = piece_type_at(p, from);
    if (!pt) return -1;

    captured = (pos_occupied(p) & BB(to)) != 0;
    color = (p->white & BB(from)) ? 1 : 0;

    if (pt == PT_PAWN && to == p->ep && (from & 7) != (to & 7)) {
        ep_captured = color ? p->ep - 8 : p->ep + 8;
        clear_square(p, ep_captured);
    }

    clear_square(p, from);
    final_pt = promo ? promo : pt;
    set_piece(p, to, final_pt, color);

    if (pt == PT_KING) {
        int file_delta = (to & 7) - (from & 7);
        if (file_delta == 2) {
            clear_square(p, CASTLE_ROOK_FROM[color][1]);
            set_piece(p, CASTLE_ROOK_TO[color][1], PT_ROOK, color);
        } else if (file_delta == -2) {
            clear_square(p, CASTLE_ROOK_FROM[color][0]);
            set_piece(p, CASTLE_ROOK_TO[color][0], PT_ROOK, color);
        }
        p->castling &= color ? ~(CR_WK | CR_WQ) : ~(CR_BK | CR_BQ);
    }

    if (pt == PT_ROOK) {
        if (from == SQ_A1) p->castling &= ~CR_WQ;
        else if (from == SQ_H1) p->castling &= ~CR_WK;
        else if (from == SQ_A8) p->castling &= ~CR_BQ;
        else if (from == SQ_H8) p->castling &= ~CR_BK;
    }

    if (to == SQ_A1) p->castling &= ~CR_WQ;
    else if (to == SQ_H1) p->castling &= ~CR_WK;
    else if (to == SQ_A8) p->castling &= ~CR_BQ;
    else if (to == SQ_H8) p->castling &= ~CR_BK;

    p->ep = -1;
    if (pt == PT_PAWN) {
        int diff = to - from;
        if (diff == 16 || diff == -16) p->ep = (from + to) / 2;
    }

    if (pt == PT_PAWN || captured || ep_captured >= 0) p->halfmove = 0;
    else p->halfmove += 1;

    if (!color) p->fullmove += 1;

    p->turn = !p->turn;
    return 0;
}

static uint64_t perft_rec(Pos *p, int depth)
{
    MoveList ml;
    uint64_t nodes = 0;
    int i;

    if (depth == 0) return 1;
    if (depth == 1) return (uint64_t)count_legal(p, 0);

    ml_init(&ml);
    gen_legal(p, &ml);
    for (i = 0; i < ml.n; i++) {
        Pos saved = *p;
        do_move(p, MV_FROM(ml.v[i]), MV_TO(ml.v[i]), MV_PROMO(ml.v[i]));
        nodes += perft_rec(p, depth - 1);
        *p = saved;
    }
    ml_free(&ml);
    return nodes;
}

/* Move */

typedef struct {
    PyObject_HEAD
    int from_square;
    int to_square;
    int promotion;   /* 0 means none */
} MoveObject;

static PyTypeObject MoveType;

/* Move allocation dominates movegen; keep it off the general allocator. */
#define MOVE_FREE_MAX 1024
static MoveObject *move_free[MOVE_FREE_MAX];
static int move_free_n = 0;

static inline PyObject *move_new_raw(int from, int to, int promo)
{
    MoveObject *m;
    if (move_free_n > 0) {
        m = move_free[--move_free_n];
        _Py_NewReference((PyObject *)m);
    } else {
        m = PyObject_New(MoveObject, &MoveType);
        if (!m) return NULL;
    }
    m->from_square = from;
    m->to_square = to;
    m->promotion = promo;
    return (PyObject *)m;
}

static void Move_dealloc(MoveObject *self)
{
    if (Py_IS_TYPE(self, &MoveType) && move_free_n < MOVE_FREE_MAX) {
        move_free[move_free_n++] = self;
        return;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static void move_freelist_clear(void)
{
    while (move_free_n > 0)
        PyObject_Free(move_free[--move_free_n]);
}

static int parse_square_str(const char *s, Py_ssize_t len, int *out)
{
    const char *f, *r;
    if (len < 2) {
        PyErr_SetString(PyExc_IndexError, "string index out of range");
        return -1;
    }
    f = strchr(FILE_CHARS, s[0]);
    r = strchr(RANK_CHARS, s[1]);
    if (!f || !s[0] || !r || !s[1]) {
        PyErr_SetString(PyExc_ValueError, "substring not found");
        return -1;
    }
    *out = (int)(f - FILE_CHARS) + (int)(r - RANK_CHARS) * 8;
    return 0;
}

static int symbol_to_piece_type(char c)
{
    switch (c) {
        case 'p': return PT_PAWN;
        case 'n': return PT_KNIGHT;
        case 'b': return PT_BISHOP;
        case 'r': return PT_ROOK;
        case 'q': return PT_QUEEN;
        case 'k': return PT_KING;
        default: return 0;
    }
}

static PyObject *Move_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    MoveObject *self = (MoveObject *)type->tp_alloc(type, 0);
    if (self) {
        self->from_square = 0;
        self->to_square = 0;
        self->promotion = 0;
    }
    return (PyObject *)self;
}

static int Move_init(MoveObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = { "from_square", "to_square", "promotion", NULL };
    PyObject *promo = Py_None;
    int from, to;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "ii|O", kwlist,
                                     &from, &to, &promo))
        return -1;

    self->from_square = from;
    self->to_square = to;
    if (promo == Py_None) {
        self->promotion = 0;
    } else {
        long v = PyLong_AsLong(promo);
        if (v == -1 && PyErr_Occurred()) return -1;
        self->promotion = (int)v;
    }
    return 0;
}

static PyObject *Move_get_from(MoveObject *self, void *c)
{
    return PyLong_FromLong(self->from_square);
}

static PyObject *Move_get_to(MoveObject *self, void *c)
{
    return PyLong_FromLong(self->to_square);
}

static PyObject *Move_get_promotion(MoveObject *self, void *c)
{
    if (!self->promotion) Py_RETURN_NONE;
    return PyLong_FromLong(self->promotion);
}

static int set_int_field(int *slot, PyObject *v, const char *name)
{
    long x;
    if (v == NULL) {
        PyErr_Format(PyExc_AttributeError, "cannot delete %s", name);
        return -1;
    }
    x = PyLong_AsLong(v);
    if (x == -1 && PyErr_Occurred()) return -1;
    *slot = (int)x;
    return 0;
}

static int Move_set_from(MoveObject *self, PyObject *v, void *c)
{
    return set_int_field(&self->from_square, v, "from_square");
}

static int Move_set_to(MoveObject *self, PyObject *v, void *c)
{
    return set_int_field(&self->to_square, v, "to_square");
}

static int Move_set_promotion(MoveObject *self, PyObject *v, void *c)
{
    if (v == Py_None) { self->promotion = 0; return 0; }
    return set_int_field(&self->promotion, v, "promotion");
}

static PyGetSetDef Move_getset[] = {
    { "from_square", (getter)Move_get_from, (setter)Move_set_from,
      "Starting square index (0-63).", NULL },
    { "to_square", (getter)Move_get_to, (setter)Move_set_to,
      "Target square index (0-63).", NULL },
    { "promotion", (getter)Move_get_promotion, (setter)Move_set_promotion,
      "Piece type to promote to, or None.", NULL },
    { NULL }
};

static Py_ssize_t move_uci_str(const MoveObject *m, char *buf)
{
    Py_ssize_t n = 0;
    if (m->from_square < 0 || m->from_square > 63 ||
        m->to_square < 0 || m->to_square > 63) {
        PyErr_Format(PyExc_IndexError, "square index out of range");
        return -1;
    }
    buf[n++] = SQ_NAMES[m->from_square][0];
    buf[n++] = SQ_NAMES[m->from_square][1];
    buf[n++] = SQ_NAMES[m->to_square][0];
    buf[n++] = SQ_NAMES[m->to_square][1];
    if (m->promotion >= 1 && m->promotion <= 6)
        buf[n++] = PIECE_CHARS[m->promotion];
    buf[n] = '\0';
    return n;
}

static PyObject *Move_uci(MoveObject *self, PyObject *Py_UNUSED(ignored))
{
    char buf[8];
    Py_ssize_t n = move_uci_str(self, buf);
    if (n < 0) return NULL;
    return PyUnicode_FromStringAndSize(buf, n);
}

static PyObject *Move_from_uci(PyTypeObject *cls, PyObject *arg)
{
    const char *s;
    Py_ssize_t len;
    int from, to, promo = 0;

    if (!PyUnicode_Check(arg)) {
        PyErr_Format(PyExc_ValueError, "Invalid UCI move: %R", arg);
        return NULL;
    }
    s = PyUnicode_AsUTF8AndSize(arg, &len);
    if (!s) return NULL;
    if (len < 4 || len > 5) {
        PyErr_Format(PyExc_ValueError, "Invalid UCI move: %U", arg);
        return NULL;
    }
    if (parse_square_str(s, 2, &from) < 0 ||
        parse_square_str(s + 2, 2, &to) < 0) {
        PyErr_Clear();
        PyErr_Format(PyExc_ValueError, "Invalid UCI move: %U", arg);
        return NULL;
    }
    if (len == 5) {
        char c = s[4];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        promo = symbol_to_piece_type(c);
        if (!promo) {
            PyErr_Format(PyExc_ValueError,
                         "Invalid promotion piece in UCI: %U", arg);
            return NULL;
        }
    }

    if (cls == &MoveType) return move_new_raw(from, to, promo);
    return PyObject_CallFunction((PyObject *)cls, "iii", from, to, promo);
}

static PyObject *Move_null(PyTypeObject *cls, PyObject *Py_UNUSED(ignored))
{
    if (cls == &MoveType) return move_new_raw(0, 0, 0);
    return PyObject_CallFunction((PyObject *)cls, "ii", 0, 0);
}

static PyObject *Move_copy(MoveObject *self, PyObject *Py_UNUSED(ignored))
{
    return move_new_raw(self->from_square, self->to_square, self->promotion);
}

static PyObject *Move_str(MoveObject *self)
{
    return Move_uci(self, NULL);
}

static PyObject *Move_repr(MoveObject *self)
{
    char buf[8];
    Py_ssize_t n = move_uci_str(self, buf);
    if (n < 0) return NULL;
    return PyUnicode_FromFormat("Move.from_uci('%s')", buf);
}

static PyObject *Move_richcompare(PyObject *a, PyObject *b, int op)
{
    if (op != Py_EQ && op != Py_NE) Py_RETURN_NOTIMPLEMENTED;
    if (!PyObject_TypeCheck(a, &MoveType) || !PyObject_TypeCheck(b, &MoveType)) {
        if (op == Py_EQ) Py_RETURN_FALSE;
        Py_RETURN_TRUE;
    }
    {
        MoveObject *x = (MoveObject *)a, *y = (MoveObject *)b;
        int eq = x->from_square == y->from_square &&
                 x->to_square == y->to_square &&
                 x->promotion == y->promotion;
        if (op == Py_NE) eq = !eq;
        return PyBool_FromLong(eq);
    }
}

static Py_hash_t Move_hash(MoveObject *self)
{
    Py_hash_t h = (Py_hash_t)(self->from_square |
                              (self->to_square << 6) |
                              (self->promotion << 12));
    h ^= h >> 7;
    h *= (Py_hash_t)0x9E3779B1;
    if (h == -1) h = -2;
    return h;
}

static int Move_bool(MoveObject *self)
{
    return self->from_square != self->to_square || self->promotion != 0;
}

static PyMethodDef Move_methods[] = {
    { "uci", (PyCFunction)Move_uci, METH_NOARGS,
      "Convert the move to its UCI string, e.g. 'e2e4' or 'e7e8q'." },
    { "from_uci", (PyCFunction)Move_from_uci, METH_O | METH_CLASS,
      "Parse a UCI move string such as 'e2e4' or 'e7e8q'." },
    { "null", (PyCFunction)Move_null, METH_NOARGS | METH_CLASS,
      "Create a null move (no movement)." },
    { "copy", (PyCFunction)Move_copy, METH_NOARGS, "Create a copy of this move." },
    { NULL }
};

static PyNumberMethods Move_as_number = { 0 };

static PyTypeObject MoveType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bmm_chess._core.Move",
    sizeof(MoveObject),
};

/* Piece */

typedef struct {
    PyObject_HEAD
    int piece_type;
    int color;
} PieceObject;

static PyTypeObject PieceType;

/* Same trick as Move. Interning is unsafe: Piece is mutable. */
#define PIECE_FREE_MAX 256
static PieceObject *piece_free[PIECE_FREE_MAX];
static int piece_free_n = 0;

static PyObject *piece_new_raw(int pt, int color)
{
    PieceObject *p;
    if (piece_free_n > 0) {
        p = piece_free[--piece_free_n];
        _Py_NewReference((PyObject *)p);
    } else {
        p = PyObject_New(PieceObject, &PieceType);
        if (!p) return NULL;
    }
    p->piece_type = pt;
    p->color = color;
    return (PyObject *)p;
}

static void Piece_dealloc(PieceObject *self)
{
    if (Py_IS_TYPE(self, &PieceType) && piece_free_n < PIECE_FREE_MAX) {
        piece_free[piece_free_n++] = self;
        return;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static void piece_freelist_clear(void)
{
    while (piece_free_n > 0)
        PyObject_Free(piece_free[--piece_free_n]);
}

static PyObject *Piece_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    PieceObject *self = (PieceObject *)type->tp_alloc(type, 0);
    if (self) { self->piece_type = 0; self->color = 1; }
    return (PyObject *)self;
}

static int Piece_init(PieceObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = { "piece_type", "color", NULL };
    PyObject *color;
    int pt, c;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "iO", kwlist, &pt, &color))
        return -1;
    c = PyObject_IsTrue(color);
    if (c < 0) return -1;
    self->piece_type = pt;
    self->color = c;
    return 0;
}

static PyObject *Piece_get_type(PieceObject *self, void *c)
{
    return PyLong_FromLong(self->piece_type);
}

static int Piece_set_type(PieceObject *self, PyObject *v, void *c)
{
    return set_int_field(&self->piece_type, v, "piece_type");
}

static PyObject *Piece_get_color(PieceObject *self, void *c)
{
    return PyBool_FromLong(self->color);
}

static int Piece_set_color(PieceObject *self, PyObject *v, void *c)
{
    int t;
    if (v == NULL) {
        PyErr_SetString(PyExc_AttributeError, "cannot delete color");
        return -1;
    }
    t = PyObject_IsTrue(v);
    if (t < 0) return -1;
    self->color = t;
    return 0;
}

static PyGetSetDef Piece_getset[] = {
    { "piece_type", (getter)Piece_get_type, (setter)Piece_set_type,
      "Type of piece (PAWN=1 through KING=6).", NULL },
    { "color", (getter)Piece_get_color, (setter)Piece_set_color,
      "Colour of piece (WHITE=True, BLACK=False).", NULL },
    { NULL }
};

static char piece_symbol_char(const PieceObject *p)
{
    char c = (p->piece_type >= 1 && p->piece_type <= 6)
             ? PIECE_CHARS[p->piece_type] : '\0';
    if (!c) return '\0';
    return p->color ? (char)(c - 'a' + 'A') : c;
}

static PyObject *Piece_symbol(PieceObject *self, PyObject *Py_UNUSED(i))
{
    char c = piece_symbol_char(self);
    if (!c) return PyUnicode_FromStringAndSize("", 0);
    return PyUnicode_FromStringAndSize(&c, 1);
}

static PyObject *Piece_unicode_symbol(PieceObject *self, PyObject *Py_UNUSED(i))
{
    if (self->piece_type < 1 || self->piece_type > 6)
        return PyUnicode_FromString("?");
    return PyUnicode_FromString(UNICODE_PIECES[self->color ? 1 : 0][self->piece_type]);
}

static PyObject *Piece_name(PieceObject *self, PyObject *Py_UNUSED(i))
{
    const char *n = (self->piece_type >= 1 && self->piece_type <= 6)
                    ? PIECE_NAMES_C[self->piece_type] : "unknown";
    return PyUnicode_FromFormat("%s %s", self->color ? "white" : "black", n);
}

static PyObject *Piece_from_symbol(PyTypeObject *cls, PyObject *arg)
{
    const char *s;
    Py_ssize_t len;
    char lower;
    int pt, color;

    if (!PyUnicode_Check(arg)) {
        PyErr_Format(PyExc_ValueError, "Invalid piece symbol: %S", arg);
        return NULL;
    }
    s = PyUnicode_AsUTF8AndSize(arg, &len);
    if (!s) return NULL;
    lower = (len == 1 && s[0] >= 'A' && s[0] <= 'Z') ? (char)(s[0] - 'A' + 'a')
            : (len == 1 ? s[0] : '\0');
    pt = lower ? symbol_to_piece_type(lower) : 0;
    if (!pt) {
        PyErr_Format(PyExc_ValueError, "Invalid piece symbol: %U", arg);
        return NULL;
    }
    color = (s[0] >= 'A' && s[0] <= 'Z');
    if (cls == &PieceType) return piece_new_raw(pt, color);
    return PyObject_CallFunction((PyObject *)cls, "iO", pt,
                                 color ? Py_True : Py_False);
}

static PyObject *Piece_copy(PieceObject *self, PyObject *Py_UNUSED(i))
{
    return piece_new_raw(self->piece_type, self->color);
}

static PyObject *Piece_str(PieceObject *self)
{
    return Piece_symbol(self, NULL);
}

static PyObject *Piece_repr(PieceObject *self)
{
    char c = piece_symbol_char(self);
    char buf[2];
    buf[0] = c;
    buf[1] = '\0';
    return PyUnicode_FromFormat("Piece.from_symbol('%s')", c ? buf : "");
}

static PyObject *Piece_richcompare(PyObject *a, PyObject *b, int op)
{
    if (op != Py_EQ && op != Py_NE) Py_RETURN_NOTIMPLEMENTED;
    if (!PyObject_TypeCheck(a, &PieceType) || !PyObject_TypeCheck(b, &PieceType)) {
        if (op == Py_EQ) Py_RETURN_FALSE;
        Py_RETURN_TRUE;
    }
    {
        PieceObject *x = (PieceObject *)a, *y = (PieceObject *)b;
        int eq = x->piece_type == y->piece_type && x->color == y->color;
        if (op == Py_NE) eq = !eq;
        return PyBool_FromLong(eq);
    }
}

static Py_hash_t Piece_hash(PieceObject *self)
{
    Py_hash_t h = (Py_hash_t)(self->piece_type * 2 + self->color);
    h = h * 1000003 + 7;
    if (h == -1) h = -2;
    return h;
}

static PyMethodDef Piece_methods[] = {
    { "symbol", (PyCFunction)Piece_symbol, METH_NOARGS,
      "Get the piece symbol: uppercase for white, lowercase for black." },
    { "unicode_symbol", (PyCFunction)Piece_unicode_symbol, METH_NOARGS,
      "Get the Unicode chess piece symbol." },
    { "name", (PyCFunction)Piece_name, METH_NOARGS,
      "Get the piece name (e.g., 'white knight')." },
    { "from_symbol", (PyCFunction)Piece_from_symbol, METH_O | METH_CLASS,
      "Create a piece from a symbol such as 'P', 'n' or 'K'; case sets colour." },
    { "copy", (PyCFunction)Piece_copy, METH_NOARGS, "Create a copy of this piece." },
    { NULL }
};

static PyTypeObject PieceType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bmm_chess._core.Piece",
    sizeof(PieceObject),
};

/* SquareSet */

typedef struct {
    PyObject_HEAD
    U64 mask;
} SquareSetObject;

static PyTypeObject SquareSetType;
static PyTypeObject SquareSetIterType;

typedef struct {
    PyObject_HEAD
    U64 mask;
} SSIterObject;

static PyObject *squareset_new_raw(U64 mask)
{
    SquareSetObject *s = PyObject_New(SquareSetObject, &SquareSetType);
    if (!s) return NULL;
    s->mask = mask;
    return (PyObject *)s;
}

/* Returns 0 and stores the mask, or 1 when `o` is not mask-like. */
static int coerce_mask(PyObject *o, U64 *out)
{
    if (PyObject_TypeCheck(o, &SquareSetType)) {
        *out = ((SquareSetObject *)o)->mask;
        return 0;
    }
    if (PyLong_Check(o)) {
        *out = PyLong_AsUnsignedLongLongMask(o);
        if (PyErr_Occurred()) return -1;
        return 0;
    }
    return 1;
}

/* Subclasses are GC heap types; tp_free routes them to PyObject_GC_Del. */
static void SS_dealloc(SquareSetObject *self)
{
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *SS_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    SquareSetObject *self = (SquareSetObject *)type->tp_alloc(type, 0);
    if (self) self->mask = 0;
    return (PyObject *)self;
}

static int mask_from_object(PyObject *o, U64 *out);

static int SS_init(SquareSetObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = { "squares", NULL };
    PyObject *m = NULL;
    U64 mask = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &m)) return -1;
    if (m) {
        int r = mask_from_object(m, &mask);
        if (r < 0) return -1;
        if (r > 0) {
            PyErr_Format(PyExc_TypeError,
                         "expected a bitboard int or an iterable of squares, not %.200s",
                         Py_TYPE(m)->tp_name);
            return -1;
        }
    }
    self->mask = mask;
    return 0;
}

static PyObject *SSIter_next(SSIterObject *it)
{
    if (!it->mask) return NULL;
    return PyLong_FromLong(pop_lsb(&it->mask));
}

static PyObject *SS_iter(SquareSetObject *self)
{
    SSIterObject *it = PyObject_New(SSIterObject, &SquareSetIterType);
    if (!it) return NULL;
    it->mask = self->mask;
    return (PyObject *)it;
}

static Py_ssize_t SS_len(SquareSetObject *self)
{
    return popcount64(self->mask);
}

static int SS_bool(SquareSetObject *self)
{
    return self->mask != 0;
}

static int SS_contains(SquareSetObject *self, PyObject *item)
{
    long sq = PyLong_AsLong(item);
    if (sq == -1 && PyErr_Occurred()) return -1;
    if (sq < 0 || sq > 63) return 0;
    return (self->mask >> sq) & 1;
}

static PyObject *SS_get_mask(SquareSetObject *self, void *c)
{
    return PyLong_FromUnsignedLongLong(self->mask);
}

static PyObject *SS_int(SquareSetObject *self)
{
    return PyLong_FromUnsignedLongLong(self->mask);
}

#define SS_BINOP(name, expr)                                            \
static PyObject *name(PyObject *a, PyObject *b)                         \
{                                                                       \
    U64 x, y;                                                           \
    if (!PyObject_TypeCheck(a, &SquareSetType)) Py_RETURN_NOTIMPLEMENTED; \
    x = ((SquareSetObject *)a)->mask;                                   \
    {                                                                   \
        int r = coerce_mask(b, &y);                                     \
        if (r < 0) return NULL;                                         \
        if (r > 0) Py_RETURN_NOTIMPLEMENTED;                            \
    }                                                                   \
    return squareset_new_raw(expr);                                     \
}

SS_BINOP(SS_or, x | y)
SS_BINOP(SS_and, x & y)
SS_BINOP(SS_xor, x ^ y)
SS_BINOP(SS_sub, x & ~y)

static PyObject *SS_richcompare(PyObject *a, PyObject *b, int op)
{
    U64 x, y;
    int r, eq;

    if (op != Py_EQ && op != Py_NE) Py_RETURN_NOTIMPLEMENTED;
    if (!PyObject_TypeCheck(a, &SquareSetType)) Py_RETURN_NOTIMPLEMENTED;
    x = ((SquareSetObject *)a)->mask;
    r = coerce_mask(b, &y);
    if (r < 0) return NULL;
    if (r > 0) Py_RETURN_NOTIMPLEMENTED;
    eq = (x == y);
    if (op == Py_NE) eq = !eq;
    return PyBool_FromLong(eq);
}

static Py_hash_t SS_hash(SquareSetObject *self)
{
    Py_hash_t h = (Py_hash_t)self->mask;
    if (h == -1) h = -2;
    return h;
}

static PyObject *SS_tolist(SquareSetObject *self, PyObject *Py_UNUSED(i))
{
    U64 bb = self->mask;
    PyObject *list = PyList_New(popcount64(bb));
    Py_ssize_t i = 0;

    if (!list) return NULL;
    while (bb) {
        PyObject *v = PyLong_FromLong(pop_lsb(&bb));
        if (!v) { Py_DECREF(list); return NULL; }
        PyList_SET_ITEM(list, i++, v);
    }
    return list;
}

static PyObject *SS_repr(SquareSetObject *self)
{
    char buf[32];
    PyOS_snprintf(buf, sizeof(buf), "SquareSet(0x%016llx)",
                  (unsigned long long)self->mask);
    return PyUnicode_FromString(buf);
}

/* SquareSet, bitboard int, or any iterable of squares. */
static int mask_from_object(PyObject *o, U64 *out)
{
    PyObject *iter, *item;
    U64 mask = 0;
    int r = coerce_mask(o, out);

    if (r <= 0) return r;

    iter = PyObject_GetIter(o);
    if (!iter) { PyErr_Clear(); return 1; }
    while ((item = PyIter_Next(iter)) != NULL) {
        long sq = PyLong_AsLong(item);
        Py_DECREF(item);
        if (sq == -1 && PyErr_Occurred()) { Py_DECREF(iter); return -1; }
        if (sq < 0 || sq > 63) {
            Py_DECREF(iter);
            PyErr_Format(PyExc_ValueError, "square out of range: %ld", sq);
            return -1;
        }
        mask |= BB(sq);
    }
    Py_DECREF(iter);
    if (PyErr_Occurred()) return -1;
    *out = mask;
    return 0;
}

static int ss_square_arg(PyObject *arg, int *sq)
{
    long v = PyLong_AsLong(arg);
    if (v == -1 && PyErr_Occurred()) return -1;
    if (v < 0 || v > 63) {
        PyErr_Format(PyExc_ValueError, "square out of range: %ld", v);
        return -1;
    }
    *sq = (int)v;
    return 0;
}

static PyObject *SS_add(SquareSetObject *self, PyObject *arg)
{
    int sq;
    if (ss_square_arg(arg, &sq) < 0) return NULL;
    self->mask |= BB(sq);
    Py_RETURN_NONE;
}

static PyObject *SS_discard(SquareSetObject *self, PyObject *arg)
{
    int sq;
    if (ss_square_arg(arg, &sq) < 0) return NULL;
    self->mask &= ~BB(sq);
    Py_RETURN_NONE;
}

static PyObject *SS_remove(SquareSetObject *self, PyObject *arg)
{
    int sq;
    if (ss_square_arg(arg, &sq) < 0) return NULL;
    if (!(self->mask & BB(sq))) {
        PyErr_Format(PyExc_KeyError, "%d", sq);
        return NULL;
    }
    self->mask &= ~BB(sq);
    Py_RETURN_NONE;
}

static PyObject *SS_pop(SquareSetObject *self, PyObject *Py_UNUSED(i))
{
    int sq;
    if (!self->mask) {
        PyErr_SetString(PyExc_KeyError, "pop from empty SquareSet");
        return NULL;
    }
    sq = ctz64(self->mask);
    self->mask &= self->mask - 1;
    return PyLong_FromLong(sq);
}

static PyObject *SS_clear(SquareSetObject *self, PyObject *Py_UNUSED(i))
{
    self->mask = 0;
    Py_RETURN_NONE;
}

static PyObject *SS_copy(SquareSetObject *self, PyObject *Py_UNUSED(i))
{
    return squareset_new_raw(self->mask);
}

static PyObject *SS_mirror(SquareSetObject *self, PyObject *Py_UNUSED(i))
{
    return squareset_new_raw(flip_vertical_bb(self->mask));
}

static PyObject *SS_from_square(PyTypeObject *cls, PyObject *arg)
{
    int sq;
    if (ss_square_arg(arg, &sq) < 0) return NULL;
    return squareset_new_raw(BB(sq));
}

/* op: 0 union, 1 intersection, 2 difference, 3 symmetric difference. */
static PyObject *ss_setop(SquareSetObject *self, PyObject *args, int op, int inplace)
{
    U64 acc = self->mask;
    Py_ssize_t i;

    for (i = 0; i < PyTuple_GET_SIZE(args); i++) {
        U64 other;
        int r = mask_from_object(PyTuple_GET_ITEM(args, i), &other);
        if (r < 0) return NULL;
        if (r > 0) {
            PyErr_SetString(PyExc_TypeError,
                            "expected a SquareSet, int or iterable of squares");
            return NULL;
        }
        switch (op) {
            case 0: acc |= other; break;
            case 1: acc &= other; break;
            case 2: acc &= ~other; break;
            default: acc ^= other; break;
        }
    }
    if (inplace) {
        self->mask = acc;
        Py_INCREF(self);
        return (PyObject *)self;
    }
    return squareset_new_raw(acc);
}

static PyObject *SS_union(SquareSetObject *s, PyObject *a) { return ss_setop(s, a, 0, 0); }
static PyObject *SS_intersection(SquareSetObject *s, PyObject *a) { return ss_setop(s, a, 1, 0); }
static PyObject *SS_difference(SquareSetObject *s, PyObject *a) { return ss_setop(s, a, 2, 0); }
static PyObject *SS_symmetric_difference(SquareSetObject *s, PyObject *a) { return ss_setop(s, a, 3, 0); }

static PyObject *SS_update(SquareSetObject *s, PyObject *a)
{
    PyObject *r = ss_setop(s, a, 0, 1);
    if (!r) return NULL;
    Py_DECREF(r);
    Py_RETURN_NONE;
}
static PyObject *SS_intersection_update(SquareSetObject *s, PyObject *a)
{
    PyObject *r = ss_setop(s, a, 1, 1);
    if (!r) return NULL;
    Py_DECREF(r);
    Py_RETURN_NONE;
}
static PyObject *SS_difference_update(SquareSetObject *s, PyObject *a)
{
    PyObject *r = ss_setop(s, a, 2, 1);
    if (!r) return NULL;
    Py_DECREF(r);
    Py_RETURN_NONE;
}
static PyObject *SS_symmetric_difference_update(SquareSetObject *s, PyObject *a)
{
    PyObject *r = ss_setop(s, a, 3, 1);
    if (!r) return NULL;
    Py_DECREF(r);
    Py_RETURN_NONE;
}

static PyObject *SS_issubset(SquareSetObject *self, PyObject *arg)
{
    U64 other;
    int r = mask_from_object(arg, &other);
    if (r < 0) return NULL;
    if (r > 0) Py_RETURN_NOTIMPLEMENTED;
    return PyBool_FromLong((self->mask & ~other) == 0);
}

static PyObject *SS_issuperset(SquareSetObject *self, PyObject *arg)
{
    U64 other;
    int r = mask_from_object(arg, &other);
    if (r < 0) return NULL;
    if (r > 0) Py_RETURN_NOTIMPLEMENTED;
    return PyBool_FromLong((other & ~self->mask) == 0);
}

static PyObject *SS_isdisjoint(SquareSetObject *self, PyObject *arg)
{
    U64 other;
    int r = mask_from_object(arg, &other);
    if (r < 0) return NULL;
    if (r > 0) Py_RETURN_NOTIMPLEMENTED;
    return PyBool_FromLong((self->mask & other) == 0);
}

/* Every subset, lowest first, ending at empty. */
typedef struct {
    PyObject_HEAD
    U64 mask;
    U64 subset;
    int done;
} RipplerObject;

static PyTypeObject RipplerType;

static PyObject *Rippler_next(RipplerObject *self)
{
    U64 current;
    if (self->done) return NULL;
    current = self->subset;
    self->subset = (self->subset - self->mask) & self->mask;
    if (!self->subset) self->done = 1;
    return PyLong_FromUnsignedLongLong(current);
}

static PyObject *SS_carry_rippler(SquareSetObject *self, PyObject *Py_UNUSED(i))
{
    RipplerObject *it = PyObject_New(RipplerObject, &RipplerType);
    if (!it) return NULL;
    it->mask = self->mask;
    it->subset = 0;
    it->done = 0;
    return (PyObject *)it;
}

static PyObject *SS_reversed(SquareSetObject *self, PyObject *Py_UNUSED(i))
{
    U64 bb = self->mask;
    PyObject *list = PyList_New(popcount64(bb));
    PyObject *it;
    Py_ssize_t i = 0;

    if (!list) return NULL;
    while (bb) {
        int sq = msb64(bb);
        PyObject *v = PyLong_FromLong(sq);
        bb ^= BB(sq);
        if (!v) { Py_DECREF(list); return NULL; }
        PyList_SET_ITEM(list, i++, v);
    }
    it = PySeqIter_New(list);
    Py_DECREF(list);
    return it;
}

static PyMethodDef SS_methods[] = {
    { "tolist", (PyCFunction)SS_tolist, METH_NOARGS,
      "The squares in this set, as a sorted list of square indices." },
    { "add", (PyCFunction)SS_add, METH_O, "Add a square to the set." },
    { "discard", (PyCFunction)SS_discard, METH_O,
      "Remove a square if present, without complaining if it is not." },
    { "remove", (PyCFunction)SS_remove, METH_O,
      "Remove a square, raising KeyError if it is not present." },
    { "pop", (PyCFunction)SS_pop, METH_NOARGS,
      "Remove and return the lowest square in the set." },
    { "clear", (PyCFunction)SS_clear, METH_NOARGS, "Remove every square." },
    { "copy", (PyCFunction)SS_copy, METH_NOARGS, "A copy of this set." },
    { "mirror", (PyCFunction)SS_mirror, METH_NOARGS,
      "A copy of this set flipped vertically." },
    { "from_square", (PyCFunction)SS_from_square, METH_O | METH_CLASS,
      "A set containing just the given square." },
    { "union", (PyCFunction)SS_union, METH_VARARGS, "Union with other square sets." },
    { "intersection", (PyCFunction)SS_intersection, METH_VARARGS,
      "Intersection with other square sets." },
    { "difference", (PyCFunction)SS_difference, METH_VARARGS,
      "Squares in this set but not the others." },
    { "symmetric_difference", (PyCFunction)SS_symmetric_difference, METH_VARARGS,
      "Squares in exactly one of the sets." },
    { "update", (PyCFunction)SS_update, METH_VARARGS, "Union in place." },
    { "intersection_update", (PyCFunction)SS_intersection_update, METH_VARARGS,
      "Intersection in place." },
    { "difference_update", (PyCFunction)SS_difference_update, METH_VARARGS,
      "Difference in place." },
    { "symmetric_difference_update", (PyCFunction)SS_symmetric_difference_update,
      METH_VARARGS, "Symmetric difference in place." },
    { "issubset", (PyCFunction)SS_issubset, METH_O,
      "Whether every square here is also in the other set." },
    { "issuperset", (PyCFunction)SS_issuperset, METH_O,
      "Whether this set contains every square of the other." },
    { "isdisjoint", (PyCFunction)SS_isdisjoint, METH_O,
      "Whether the two sets share no squares." },
    { "carry_rippler", (PyCFunction)SS_carry_rippler, METH_NOARGS,
      "Iterate every subset of this set as a bitboard." },
    { "__reversed__", (PyCFunction)SS_reversed, METH_NOARGS, NULL },
    { NULL }
};

static PyGetSetDef SS_getset[] = {
    { "mask", (getter)SS_get_mask, NULL, "The underlying bitboard.", NULL },
    { NULL }
};

static PyNumberMethods SS_as_number = { 0 };
static PySequenceMethods SS_as_sequence = { 0 };

static PyTypeObject SquareSetType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bmm_chess._core.SquareSet",
    sizeof(SquareSetObject),
};

static PyTypeObject SquareSetIterType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bmm_chess._core.SquareSetIterator",
    sizeof(SSIterObject),
};

/* move iterator (shared by the pseudo-legal and legal generators) */

typedef struct {
    PyObject_HEAD
    int *moves;
    int n;
    int i;
} MoveIterObject;

static PyTypeObject MoveIterType;

/* Takes ownership of `ml`'s storage, copying out of the inline buffer. */
static PyObject *moveiter_from_list(MoveList *ml)
{
    MoveIterObject *it = PyObject_New(MoveIterObject, &MoveIterType);
    if (!it) { ml_free(ml); return NULL; }
    it->n = ml->n;
    it->i = 0;
    it->moves = NULL;
    if (ml->n > 0) {
        it->moves = (int *)malloc(sizeof(int) * (size_t)ml->n);
        if (!it->moves) {
            ml_free(ml);
            Py_DECREF(it);
            return PyErr_NoMemory();
        }
        memcpy(it->moves, ml->v, sizeof(int) * (size_t)ml->n);
    }
    ml_free(ml);
    return (PyObject *)it;
}

static void MoveIter_dealloc(MoveIterObject *self)
{
    free(self->moves);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *MoveIter_next(MoveIterObject *self)
{
    int m;
    if (self->i >= self->n) return NULL;
    m = self->moves[self->i++];
    return move_new_raw(MV_FROM(m), MV_TO(m), MV_PROMO(m));
}

static PyObject *MoveIter_len(MoveIterObject *self, PyObject *Py_UNUSED(i))
{
    return PyLong_FromLong(self->n - self->i);
}

static PyMethodDef MoveIter_methods[] = {
    { "__length_hint__", (PyCFunction)MoveIter_len, METH_NOARGS, NULL },
    { NULL }
};

static PyTypeObject MoveIterType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bmm_chess._core.MoveIterator",
    sizeof(MoveIterObject),
};

/* Board */

typedef struct {
    PyObject_HEAD
    Pos p;
    Undo *stack;
    Py_ssize_t stack_len;
    Py_ssize_t stack_cap;
} BoardObject;

typedef struct {
    PyObject_HEAD
    BoardObject *board;
    int pseudo;      /* 1 when this generator yields pseudo-legal moves */
} LMGObject;

static PyTypeObject BoardType;
static PyTypeObject LMGType;

static void pos_clear(Pos *p)
{
    memset(p, 0, sizeof(*p));
    p->turn = 1;
    p->castling = CR_ALL;
    p->ep = -1;
    p->halfmove = 0;
    p->fullmove = 1;
}

static int board_push_undo(BoardObject *b, int from, int to, int promo)
{
    if (b->stack_len == b->stack_cap) {
        Py_ssize_t cap = b->stack_cap ? b->stack_cap * 2 : 64;
        Undo *s = (Undo *)PyMem_Realloc(b->stack, sizeof(Undo) * (size_t)cap);
        if (!s) { PyErr_NoMemory(); return -1; }
        b->stack = s;
        b->stack_cap = cap;
    }
    b->stack[b->stack_len].pos = b->p;
    b->stack[b->stack_len].from = from;
    b->stack[b->stack_len].to = to;
    b->stack[b->stack_len].promo = promo;
    b->stack_len++;
    return 0;
}

/* A Move, or anything with from_square/to_square/promotion. */
static int get_move_fields(PyObject *o, int *from, int *to, int *promo)
{
    PyObject *v;
    long x;

    if (PyObject_TypeCheck(o, &MoveType)) {
        MoveObject *m = (MoveObject *)o;
        *from = m->from_square;
        *to = m->to_square;
        *promo = m->promotion;
        return 0;
    }

    v = PyObject_GetAttrString(o, "from_square");
    if (!v) return -1;
    x = PyLong_AsLong(v);
    Py_DECREF(v);
    if (x == -1 && PyErr_Occurred()) return -1;
    *from = (int)x;

    v = PyObject_GetAttrString(o, "to_square");
    if (!v) return -1;
    x = PyLong_AsLong(v);
    Py_DECREF(v);
    if (x == -1 && PyErr_Occurred()) return -1;
    *to = (int)x;

    *promo = 0;
    v = PyObject_GetAttrString(o, "promotion");
    if (!v) {
        PyErr_Clear();
    } else {
        if (v != Py_None) {
            x = PyLong_AsLong(v);
            if (x == -1 && PyErr_Occurred()) { Py_DECREF(v); return -1; }
            *promo = (int)x;
        }
        Py_DECREF(v);
    }
    return 0;
}

static int check_square_range(int sq)
{
    if (sq < 0 || sq > 63) {
        PyErr_Format(PyExc_ValueError, "square out of range: %d", sq);
        return -1;
    }
    return 0;
}

static int move_in_range(int from, int to)
{
    if (from < 0 || from > 63 || to < 0 || to > 63) {
        PyErr_Format(PyExc_ValueError,
                     "move squares out of range: %d, %d", from, to);
        return -1;
    }
    return 0;
}

static int arg_color(PyObject *o)
{
    if (o == Py_True) return 1;
    if (o == Py_False) return 0;
    return PyObject_IsTrue(o);
}

static PyObject *Board_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    BoardObject *self = (BoardObject *)type->tp_alloc(type, 0);
    if (self) {
        pos_clear(&self->p);
        self->stack = NULL;
        self->stack_len = 0;
        self->stack_cap = 0;
    }
    return (PyObject *)self;
}

static void Board_dealloc(BoardObject *self)
{
    PyMem_Free(self->stack);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static int board_set_fen(BoardObject *self, PyObject *fen_obj);

static int Board_init(BoardObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = { "fen", NULL };
    PyObject *fen = NULL;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &fen))
        return -1;

    pos_clear(&self->p);
    self->stack_len = 0;

    if (fen == NULL) {
        PyObject *def = PyUnicode_FromString(STARTING_FEN_C);
        int r;
        if (!def) return -1;
        r = board_set_fen(self, def);
        Py_DECREF(def);
        return r;
    }
    if (fen == Py_None) return 0;
    {
        int truthy = PyObject_IsTrue(fen);
        if (truthy < 0) return -1;
        if (!truthy) return 0;
    }
    return board_set_fen(self, fen);
}

/* --- attributes --- */

static PyObject *Board_get_turn(BoardObject *self, void *c)
{
    return PyBool_FromLong(self->p.turn);
}

static int Board_set_turn(BoardObject *self, PyObject *v, void *c)
{
    int t;
    if (v == NULL) {
        PyErr_SetString(PyExc_AttributeError, "cannot delete turn");
        return -1;
    }
    t = PyObject_IsTrue(v);
    if (t < 0) return -1;
    self->p.turn = t;
    return 0;
}

static PyObject *Board_get_ep(BoardObject *self, void *c)
{
    if (self->p.ep < 0) Py_RETURN_NONE;
    return PyLong_FromLong(self->p.ep);
}

static int Board_set_ep(BoardObject *self, PyObject *v, void *c)
{
    long x;
    if (v == NULL) {
        PyErr_SetString(PyExc_AttributeError, "cannot delete ep_square");
        return -1;
    }
    if (v == Py_None) { self->p.ep = -1; return 0; }
    x = PyLong_AsLong(v);
    if (x == -1 && PyErr_Occurred()) return -1;
    if (x < 0 || x > 63) {
        PyErr_Format(PyExc_ValueError, "ep_square out of range: %ld", x);
        return -1;
    }
    self->p.ep = (int)x;
    return 0;
}

#define INT_ATTR(name, field)                                           \
static PyObject *Board_get_##name(BoardObject *self, void *c)           \
{ return PyLong_FromLong(self->p.field); }                              \
static int Board_set_##name(BoardObject *self, PyObject *v, void *c)    \
{ return set_int_field(&self->p.field, v, #name); }

INT_ATTR(castling_rights, castling)
INT_ATTR(halfmove_clock, halfmove)
INT_ATTR(fullmove_number, fullmove)

#define BB_ATTR(name, field)                                            \
static PyObject *Board_get_##name(BoardObject *self, void *c)           \
{ return PyLong_FromUnsignedLongLong(self->p.field); }                  \
static int Board_set_##name(BoardObject *self, PyObject *v, void *c)    \
{                                                                       \
    U64 m;                                                              \
    if (v == NULL) {                                                    \
        PyErr_SetString(PyExc_AttributeError, "cannot delete " #name);  \
        return -1;                                                      \
    }                                                                   \
    m = PyLong_AsUnsignedLongLongMask(v);                               \
    if (PyErr_Occurred()) return -1;                                    \
    self->p.field = m;                                                  \
    return 0;                                                           \
}

BB_ATTR(_pawns, pawns)
BB_ATTR(_knights, knights)
BB_ATTR(_bishops, bishops)
BB_ATTR(_rooks, rooks)
BB_ATTR(_queens, queens)
BB_ATTR(_kings, kings)
BB_ATTR(_white, white)
BB_ATTR(_black, black)

static PyObject *Board_get_occupied(BoardObject *self, void *c)
{
    return PyLong_FromUnsignedLongLong(pos_occupied(&self->p));
}

static PyObject *Board_get_checkers_mask(BoardObject *self, void *c)
{
    int ks = king_square(&self->p, self->p.turn);
    if (ks < 0) return PyLong_FromLong(0);
    return PyLong_FromUnsignedLongLong(
        attackers_to(&self->p, !self->p.turn, ks, pos_occupied(&self->p)));
}

static PyObject *board_move_generator(BoardObject *self, int pseudo)
{
    LMGObject *lmg = PyObject_New(LMGObject, &LMGType);
    if (!lmg) return NULL;
    Py_INCREF(self);
    lmg->board = self;
    lmg->pseudo = pseudo;
    return (PyObject *)lmg;
}

static PyObject *Board_get_legal_moves(BoardObject *self, void *c)
{
    return board_move_generator(self, 0);
}

static PyObject *Board_get_pseudo_legal_moves(BoardObject *self, void *c)
{
    return board_move_generator(self, 1);
}

/* --- queries --- */

static PyObject *Board_occupied_co(BoardObject *self, PyObject *arg)
{
    int c = arg_color(arg);
    if (c < 0) return NULL;
    return PyLong_FromUnsignedLongLong(c ? self->p.white : self->p.black);
}

static U64 pieces_mask_of(const Pos *p, int pt, int color)
{
    U64 t;
    switch (pt) {
        case PT_PAWN:   t = p->pawns;   break;
        case PT_KNIGHT: t = p->knights; break;
        case PT_BISHOP: t = p->bishops; break;
        case PT_ROOK:   t = p->rooks;   break;
        case PT_QUEEN:  t = p->queens;  break;
        case PT_KING:   t = p->kings;   break;
        default: return 0;
    }
    return t & (color ? p->white : p->black);
}

static PyObject *Board_pieces_mask(BoardObject *self, PyObject *args)
{
    PyObject *color_obj;
    int pt, c;

    if (!PyArg_ParseTuple(args, "iO", &pt, &color_obj)) return NULL;
    c = arg_color(color_obj);
    if (c < 0) return NULL;
    return PyLong_FromUnsignedLongLong(pieces_mask_of(&self->p, pt, c));
}

static PyObject *Board_pieces(BoardObject *self, PyObject *args)
{
    PyObject *color_obj;
    int pt, c;

    if (!PyArg_ParseTuple(args, "iO", &pt, &color_obj)) return NULL;
    c = arg_color(color_obj);
    if (c < 0) return NULL;
    return squareset_new_raw(pieces_mask_of(&self->p, pt, c));
}

static PyObject *Board_get_piece_bb(BoardObject *self, PyObject *arg)
{
    long pt = PyLong_AsLong(arg);
    const Pos *p = &self->p;
    U64 v = 0;

    if (pt == -1 && PyErr_Occurred()) return NULL;
    switch (pt) {
        case PT_PAWN:   v = p->pawns;   break;
        case PT_KNIGHT: v = p->knights; break;
        case PT_BISHOP: v = p->bishops; break;
        case PT_ROOK:   v = p->rooks;   break;
        case PT_QUEEN:  v = p->queens;  break;
        case PT_KING:   v = p->kings;   break;
        default: v = 0; break;
    }
    return PyLong_FromUnsignedLongLong(v);
}

static PyObject *Board_set_piece_bb(BoardObject *self, PyObject *args)
{
    Pos *p = &self->p;
    PyObject *bb_obj;
    U64 bb;
    int pt;

    if (!PyArg_ParseTuple(args, "iO", &pt, &bb_obj)) return NULL;
    bb = PyLong_AsUnsignedLongLongMask(bb_obj);
    if (PyErr_Occurred()) return NULL;
    switch (pt) {
        case PT_PAWN:   p->pawns = bb;   break;
        case PT_KNIGHT: p->knights = bb; break;
        case PT_BISHOP: p->bishops = bb; break;
        case PT_ROOK:   p->rooks = bb;   break;
        case PT_QUEEN:  p->queens = bb;  break;
        case PT_KING:   p->kings = bb;   break;
        default: break;
    }
    Py_RETURN_NONE;
}

static PyObject *Board_piece_at(BoardObject *self, PyObject *arg)
{
    long sq = PyLong_AsLong(arg);
    int pt;

    if (sq == -1 && PyErr_Occurred()) return NULL;
    if (check_square_range((int)sq) < 0) return NULL;
    pt = piece_type_at(&self->p, (int)sq);
    if (!pt) Py_RETURN_NONE;
    return piece_new_raw(pt, (self->p.white & BB(sq)) ? 1 : 0);
}

static PyObject *Board_piece_type_at(BoardObject *self, PyObject *arg)
{
    long sq = PyLong_AsLong(arg);
    int pt;

    if (sq == -1 && PyErr_Occurred()) return NULL;
    if (check_square_range((int)sq) < 0) return NULL;
    pt = piece_type_at(&self->p, (int)sq);
    if (!pt) Py_RETURN_NONE;
    return PyLong_FromLong(pt);
}

static PyObject *Board_color_at(BoardObject *self, PyObject *arg)
{
    long sq = PyLong_AsLong(arg);
    U64 m;

    if (sq == -1 && PyErr_Occurred()) return NULL;
    if (check_square_range((int)sq) < 0) return NULL;
    m = BB(sq);
    if (self->p.white & m) Py_RETURN_TRUE;
    if (self->p.black & m) Py_RETURN_FALSE;
    Py_RETURN_NONE;
}

static PyObject *Board_piece_map(BoardObject *self, PyObject *Py_UNUSED(i))
{
    PyObject *d = PyDict_New();
    U64 occ;

    if (!d) return NULL;
    occ = pos_occupied(&self->p);
    while (occ) {
        int sq = pop_lsb(&occ);
        PyObject *key = PyLong_FromLong(sq);
        PyObject *val = piece_new_raw(piece_type_at(&self->p, sq),
                                      (self->p.white & BB(sq)) ? 1 : 0);
        int rc;
        if (!key || !val) {
            Py_XDECREF(key); Py_XDECREF(val); Py_DECREF(d);
            return NULL;
        }
        rc = PyDict_SetItem(d, key, val);
        Py_DECREF(key);
        Py_DECREF(val);
        if (rc < 0) { Py_DECREF(d); return NULL; }
    }
    return d;
}

static PyObject *Board_clear_square(BoardObject *self, PyObject *arg)
{
    long sq = PyLong_AsLong(arg);
    if (sq == -1 && PyErr_Occurred()) return NULL;
    if (check_square_range((int)sq) < 0) return NULL;
    clear_square(&self->p, (int)sq);
    Py_RETURN_NONE;
}

static PyObject *Board_set_piece_at(BoardObject *self, PyObject *args)
{
    PyObject *color_obj;
    int sq, pt, c;

    if (!PyArg_ParseTuple(args, "iiO", &sq, &pt, &color_obj)) return NULL;
    if (check_square_range(sq) < 0) return NULL;
    c = arg_color(color_obj);
    if (c < 0) return NULL;
    set_piece(&self->p, sq, pt, c);
    Py_RETURN_NONE;
}

static PyObject *Board_remove_piece_at(BoardObject *self, PyObject *arg)
{
    long sq = PyLong_AsLong(arg);
    int pt;
    PyObject *piece;

    if (sq == -1 && PyErr_Occurred()) return NULL;
    if (check_square_range((int)sq) < 0) return NULL;
    pt = piece_type_at(&self->p, (int)sq);
    if (!pt) Py_RETURN_NONE;
    piece = piece_new_raw(pt, (self->p.white & BB(sq)) ? 1 : 0);
    if (!piece) return NULL;
    clear_square(&self->p, (int)sq);
    return piece;
}

static PyObject *Board_king(BoardObject *self, PyObject *arg)
{
    int c = arg_color(arg);
    int ks;
    if (c < 0) return NULL;
    ks = king_square(&self->p, c);
    if (ks < 0) Py_RETURN_NONE;
    return PyLong_FromLong(ks);
}

static PyObject *Board_attackers_mask(BoardObject *self, PyObject *args)
{
    PyObject *color_obj;
    int sq, c;

    if (!PyArg_ParseTuple(args, "Oi", &color_obj, &sq)) return NULL;
    if (check_square_range(sq) < 0) return NULL;
    c = arg_color(color_obj);
    if (c < 0) return NULL;
    return PyLong_FromUnsignedLongLong(
        attackers_to(&self->p, c, sq, pos_occupied(&self->p)));
}

static PyObject *Board_attackers(BoardObject *self, PyObject *args)
{
    PyObject *color_obj;
    int sq, c;

    if (!PyArg_ParseTuple(args, "Oi", &color_obj, &sq)) return NULL;
    if (check_square_range(sq) < 0) return NULL;
    c = arg_color(color_obj);
    if (c < 0) return NULL;
    return squareset_new_raw(
        attackers_to(&self->p, c, sq, pos_occupied(&self->p)));
}

static PyObject *Board_is_attacked_by(BoardObject *self, PyObject *args)
{
    PyObject *color_obj;
    int sq, c;

    if (!PyArg_ParseTuple(args, "Oi", &color_obj, &sq)) return NULL;
    if (check_square_range(sq) < 0) return NULL;
    c = arg_color(color_obj);
    if (c < 0) return NULL;
    return PyBool_FromLong(
        attackers_to(&self->p, c, sq, pos_occupied(&self->p)) != 0);
}

static PyObject *Board_attacks_mask(BoardObject *self, PyObject *arg)
{
    long sq = PyLong_AsLong(arg);
    const Pos *p = &self->p;
    U64 occ;
    int pt;

    if (sq == -1 && PyErr_Occurred()) return NULL;
    if (check_square_range((int)sq) < 0) return NULL;
    pt = piece_type_at(p, (int)sq);
    if (!pt) return PyLong_FromLong(0);
    occ = pos_occupied(p);
    switch (pt) {
        case PT_PAWN:
            return PyLong_FromUnsignedLongLong(
                PAWN_ATT[(p->white & BB(sq)) ? 1 : 0][sq]);
        case PT_KNIGHT: return PyLong_FromUnsignedLongLong(KNIGHT_ATT[sq]);
        case PT_BISHOP: return PyLong_FromUnsignedLongLong(bishop_attacks((int)sq, occ));
        case PT_ROOK:   return PyLong_FromUnsignedLongLong(rook_attacks((int)sq, occ));
        case PT_QUEEN:  return PyLong_FromUnsignedLongLong(queen_attacks((int)sq, occ));
        case PT_KING:   return PyLong_FromUnsignedLongLong(KING_ATT[sq]);
    }
    return PyLong_FromLong(0);
}

static int board_is_check(const Pos *p)
{
    int ks = king_square(p, p->turn);
    /* No king: report check so end-state logic gives the opponent the win. */
    if (ks < 0) return 1;
    return square_attacked(p, !p->turn, ks, pos_occupied(p));
}

static PyObject *Board_is_check(BoardObject *self, PyObject *Py_UNUSED(i))
{
    return PyBool_FromLong(board_is_check(&self->p));
}

static PyObject *Board_checkers(BoardObject *self, PyObject *Py_UNUSED(i))
{
    int ks = king_square(&self->p, self->p.turn);
    if (ks < 0) return squareset_new_raw(0);
    return squareset_new_raw(
        attackers_to(&self->p, !self->p.turn, ks, pos_occupied(&self->p)));
}

static PyObject *Board_is_into_check(BoardObject *self, PyObject *arg)
{
    int from, to, promo;
    int ks = king_square(&self->p, self->p.turn);

    if (get_move_fields(arg, &from, &to, &promo) < 0) return NULL;
    if (ks < 0) Py_RETURN_TRUE;
    if (move_in_range(from, to) < 0) return NULL;
    return PyBool_FromLong(king_attacked_after(&self->p, from, to, promo, ks));
}

static PyObject *Board_pinned_mask(BoardObject *self, PyObject *arg)
{
    long ks = PyLong_AsLong(arg);
    if (ks == -1 && PyErr_Occurred()) return NULL;
    if (check_square_range((int)ks) < 0) return NULL;
    return PyLong_FromUnsignedLongLong(pinned_mask(&self->p, (int)ks));
}

static PyObject *Board_is_checkmate(BoardObject *self, PyObject *Py_UNUSED(i))
{
    if (!board_is_check(&self->p)) Py_RETURN_FALSE;
    return PyBool_FromLong(count_legal(&self->p, 1) == 0);
}

static PyObject *Board_is_stalemate(BoardObject *self, PyObject *Py_UNUSED(i))
{
    if (king_square(&self->p, self->p.turn) < 0) Py_RETURN_FALSE;
    if (board_is_check(&self->p)) Py_RETURN_FALSE;
    return PyBool_FromLong(count_legal(&self->p, 1) == 0);
}

static int board_insufficient_material(const Pos *p)
{
    int knights, bishops;

    if (king_square(p, 1) < 0 || king_square(p, 0) < 0) return 0;
    if (pos_occupied(p) == p->kings) return 1;

    knights = popcount64(p->knights);
    bishops = popcount64(p->bishops);

    if (p->pawns == 0 && p->rooks == 0 && p->queens == 0) {
        if (knights + bishops <= 1) return 1;
        if (knights == 0 && bishops == 2) {
            U64 wb = p->bishops & p->white;
            U64 bb = p->bishops & p->black;
            if (popcount64(wb) == 1 && popcount64(bb) == 1) {
                int w = ctz64(wb), b = ctz64(bb);
                if ((((w >> 3) + (w & 7)) & 1) == (((b >> 3) + (b & 7)) & 1))
                    return 1;
            }
        }
    }
    return 0;
}

static PyObject *Board_is_insufficient_material(BoardObject *self, PyObject *Py_UNUSED(i))
{
    return PyBool_FromLong(board_insufficient_material(&self->p));
}

static PyObject *Board_is_fifty_moves(BoardObject *self, PyObject *Py_UNUSED(i))
{
    return PyBool_FromLong(self->p.halfmove >= 100);
}

static PyObject *Board_is_seventyfive_moves(BoardObject *self, PyObject *Py_UNUSED(i))
{
    return PyBool_FromLong(self->p.halfmove >= 150);
}

static PyObject *Board_has_kingside_castling_rights(BoardObject *self, PyObject *arg)
{
    int c = arg_color(arg);
    if (c < 0) return NULL;
    return PyBool_FromLong((self->p.castling & (c ? CR_WK : CR_BK)) != 0);
}

static PyObject *Board_has_queenside_castling_rights(BoardObject *self, PyObject *arg)
{
    int c = arg_color(arg);
    if (c < 0) return NULL;
    return PyBool_FromLong((self->p.castling & (c ? CR_WQ : CR_BQ)) != 0);
}

static PyObject *Board_has_castling_rights(BoardObject *self, PyObject *arg)
{
    int c = arg_color(arg);
    if (c < 0) return NULL;
    return PyBool_FromLong(
        (self->p.castling & (c ? (CR_WK | CR_WQ) : (CR_BK | CR_BQ))) != 0);
}

static PyObject *Board_can_castle_kingside(BoardObject *self, PyObject *arg)
{
    int c = arg_color(arg);
    if (c < 0) return NULL;
    return PyBool_FromLong(can_castle(&self->p, c, 1));
}

static PyObject *Board_can_castle_queenside(BoardObject *self, PyObject *arg)
{
    int c = arg_color(arg);
    if (c < 0) return NULL;
    return PyBool_FromLong(can_castle(&self->p, c, 0));
}

/* --- move predicates --- */

static int board_is_en_passant(const Pos *p, int from, int to)
{
    if (p->ep < 0 || to != p->ep) return 0;
    if (piece_type_at(p, from) != PT_PAWN) return 0;
    /* Diagonal onto an empty square; a straight push is not ep. */
    return (from & 7) != (to & 7) && !(pos_occupied(p) & BB(to));
}

static int board_is_castling(const Pos *p, int from, int to)
{
    int d;
    if (piece_type_at(p, from) != PT_KING) return 0;
    d = (to & 7) - (from & 7);
    return d > 1 || d < -1;
}

static PyObject *Board_is_capture(BoardObject *self, PyObject *arg)
{
    int from, to, promo;
    if (get_move_fields(arg, &from, &to, &promo) < 0) return NULL;
    if (move_in_range(from, to) < 0) return NULL;
    if (pos_occupied(&self->p) & BB(to)) Py_RETURN_TRUE;
    return PyBool_FromLong(board_is_en_passant(&self->p, from, to));
}

static PyObject *Board_is_en_passant(BoardObject *self, PyObject *arg)
{
    int from, to, promo;
    if (get_move_fields(arg, &from, &to, &promo) < 0) return NULL;
    if (move_in_range(from, to) < 0) return NULL;
    return PyBool_FromLong(board_is_en_passant(&self->p, from, to));
}

static PyObject *Board_is_castling(BoardObject *self, PyObject *arg)
{
    int from, to, promo;
    if (get_move_fields(arg, &from, &to, &promo) < 0) return NULL;
    if (move_in_range(from, to) < 0) return NULL;
    return PyBool_FromLong(board_is_castling(&self->p, from, to));
}

static PyObject *Board_is_kingside_castling(BoardObject *self, PyObject *arg)
{
    int from, to, promo;
    if (get_move_fields(arg, &from, &to, &promo) < 0) return NULL;
    if (move_in_range(from, to) < 0) return NULL;
    return PyBool_FromLong(board_is_castling(&self->p, from, to) &&
                           (to & 7) > (from & 7));
}

static PyObject *Board_is_queenside_castling(BoardObject *self, PyObject *arg)
{
    int from, to, promo;
    if (get_move_fields(arg, &from, &to, &promo) < 0) return NULL;
    if (move_in_range(from, to) < 0) return NULL;
    return PyBool_FromLong(board_is_castling(&self->p, from, to) &&
                           (to & 7) < (from & 7));
}

static PyObject *Board_is_legal(BoardObject *self, PyObject *arg)
{
    MoveList ml;
    int from, to, promo, i, found = 0, ks;

    if (get_move_fields(arg, &from, &to, &promo) < 0) return NULL;
    if (move_in_range(from, to) < 0) return NULL;

    ml_init(&ml);
    gen_pseudo(&self->p, &ml);
    for (i = 0; i < ml.n; i++) {
        if (ml.v[i] == MV(from, to, promo)) { found = 1; break; }
    }
    ml_free(&ml);
    if (!found) Py_RETURN_FALSE;

    ks = king_square(&self->p, self->p.turn);
    if (ks < 0) Py_RETURN_FALSE;
    return PyBool_FromLong(!king_attacked_after(&self->p, from, to, promo, ks));
}

/* --- make / unmake --- */

static PyObject *Board_push(BoardObject *self, PyObject *arg)
{
    int from, to, promo;

    if (get_move_fields(arg, &from, &to, &promo) < 0) return NULL;
    if (move_in_range(from, to) < 0) return NULL;

    /* Validate first, so a rejected move leaves the stack clean. */
    if (!(from == to && promo == 0) && !piece_type_at(&self->p, from)) {
        PyErr_Format(PyExc_ValueError, "No piece at %s", SQ_NAMES[from]);
        return NULL;
    }
    if (board_push_undo(self, from, to, promo) < 0) return NULL;
    do_move(&self->p, from, to, promo);
    Py_RETURN_NONE;
}

static PyObject *Board_push_uci(BoardObject *self, PyObject *arg)
{
    PyObject *move = Move_from_uci(&MoveType, arg);
    PyObject *r;

    if (!move) return NULL;
    r = Board_push(self, move);
    if (!r) { Py_DECREF(move); return NULL; }
    Py_DECREF(r);
    return move;
}

static PyObject *Board_pop(BoardObject *self, PyObject *Py_UNUSED(i))
{
    Undo *u;

    if (self->stack_len == 0) {
        PyErr_SetString(PyExc_IndexError, "Move stack is empty");
        return NULL;
    }
    u = &self->stack[--self->stack_len];
    self->p = u->pos;
    return move_new_raw(u->from, u->to, u->promo);
}

static PyObject *Board_peek(BoardObject *self, PyObject *Py_UNUSED(i))
{
    Undo *u;
    if (self->stack_len == 0) Py_RETURN_NONE;
    u = &self->stack[self->stack_len - 1];
    return move_new_raw(u->from, u->to, u->promo);
}

static PyObject *Board_move_stack(BoardObject *self, PyObject *Py_UNUSED(i))
{
    PyObject *list = PyList_New(self->stack_len);
    Py_ssize_t i;

    if (!list) return NULL;
    for (i = 0; i < self->stack_len; i++) {
        PyObject *m = move_new_raw(self->stack[i].from, self->stack[i].to,
                                   self->stack[i].promo);
        if (!m) { Py_DECREF(list); return NULL; }
        PyList_SET_ITEM(list, i, m);
    }
    return list;
}

static PyObject *Board_clear_stack(BoardObject *self, PyObject *Py_UNUSED(i))
{
    self->stack_len = 0;
    Py_RETURN_NONE;
}

static PyObject *Board_gives_check(BoardObject *self, PyObject *arg)
{
    Pos saved;
    int from, to, promo, result;

    if (get_move_fields(arg, &from, &to, &promo) < 0) return NULL;
    if (move_in_range(from, to) < 0) return NULL;
    if (!(from == to && promo == 0) && !piece_type_at(&self->p, from)) {
        PyErr_Format(PyExc_ValueError, "No piece at %s", SQ_NAMES[from]);
        return NULL;
    }
    saved = self->p;
    do_move(&self->p, from, to, promo);
    result = board_is_check(&self->p);
    self->p = saved;
    return PyBool_FromLong(result);
}

/* --- repetition and outcomes --- */

static int same_position(const Pos *a, const Pos *b)
{
    return a->pawns == b->pawns && a->knights == b->knights &&
           a->bishops == b->bishops && a->rooks == b->rooks &&
           a->queens == b->queens && a->kings == b->kings &&
           a->white == b->white && a->black == b->black &&
           a->turn == b->turn && a->castling == b->castling &&
           a->ep == b->ep;
}

/* Each undo frame holds the position before its move. */
static int board_is_repetition(const BoardObject *b, int count)
{
    int occurrences = 1;
    Py_ssize_t i;

    for (i = b->stack_len - 1; i >= 0; i--) {
        if (same_position(&b->stack[i].pos, &b->p)) {
            occurrences++;
            if (occurrences >= count) return 1;
        }
    }
    return occurrences >= count;
}

static PyObject *Board_is_repetition(BoardObject *self, PyObject *args)
{
    int count = 3;
    if (!PyArg_ParseTuple(args, "|i", &count)) return NULL;
    return PyBool_FromLong(board_is_repetition(self, count));
}

static PyObject *Board_is_fivefold_repetition(BoardObject *self, PyObject *Py_UNUSED(i))
{
    return PyBool_FromLong(board_is_repetition(self, 5));
}

static PyObject *Board_can_claim_threefold_repetition(BoardObject *self, PyObject *Py_UNUSED(i))
{
    return PyBool_FromLong(board_is_repetition(self, 3));
}

static PyObject *Board_can_claim_fifty_moves(BoardObject *self, PyObject *Py_UNUSED(i))
{
    return PyBool_FromLong(self->p.halfmove >= 100);
}

static PyObject *Board_can_claim_draw(BoardObject *self, PyObject *Py_UNUSED(i))
{
    if (self->p.halfmove >= 100) Py_RETURN_TRUE;
    return PyBool_FromLong(board_is_repetition(self, 3));
}

static PyObject *Board_position_hash(BoardObject *self, PyObject *Py_UNUSED(i))
{
    const Pos *p = &self->p;
    U64 h = p->pawns;
    h = h * 0x100000001B3ULL ^ p->knights;
    h = h * 0x100000001B3ULL ^ p->bishops;
    h = h * 0x100000001B3ULL ^ p->rooks;
    h = h * 0x100000001B3ULL ^ p->queens;
    h = h * 0x100000001B3ULL ^ p->kings;
    h = h * 0x100000001B3ULL ^ p->white;
    h = h * 0x100000001B3ULL ^ p->black;
    h = h * 0x100000001B3ULL ^ (U64)(p->turn + 2 * p->castling + 64 * (p->ep + 1));
    h ^= h >> 33;
    return PyLong_FromLongLong((long long)(h & 0x7FFFFFFFFFFFFFFFULL));
}

static int board_checkmate(const Pos *p)
{
    return board_is_check(p) && count_legal(p, 1) == 0;
}

static int board_stalemate(const Pos *p)
{
    if (king_square(p, p->turn) < 0) return 0;
    return !board_is_check(p) && count_legal(p, 1) == 0;
}

static PyObject *Board_outcome(BoardObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = { "claim_draw", NULL };
    PyObject *claim = Py_False;
    int claim_draw;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &claim))
        return NULL;
    claim_draw = PyObject_IsTrue(claim);
    if (claim_draw < 0) return NULL;

    if (board_checkmate(&self->p)) return PyUnicode_FromString("checkmate");
    if (board_stalemate(&self->p)) return PyUnicode_FromString("stalemate");
    if (board_insufficient_material(&self->p))
        return PyUnicode_FromString("insufficient_material");
    if (self->p.halfmove >= 100) return PyUnicode_FromString("fifty_moves");
    if (board_is_repetition(self, 5))
        return PyUnicode_FromString("fivefold_repetition");
    if (claim_draw && board_is_repetition(self, 3))
        return PyUnicode_FromString("threefold_repetition");
    Py_RETURN_NONE;
}

static PyObject *Board_is_game_over(BoardObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *outcome = Board_outcome(self, args, kwds);
    int over;
    if (!outcome) return NULL;
    over = outcome != Py_None;
    Py_DECREF(outcome);
    return PyBool_FromLong(over);
}

static PyObject *Board_result(BoardObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = { "claim_draw", NULL };
    PyObject *claim = Py_False;
    int claim_draw;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &claim))
        return NULL;
    claim_draw = PyObject_IsTrue(claim);
    if (claim_draw < 0) return NULL;

    if (board_checkmate(&self->p))
        return PyUnicode_FromString(self->p.turn ? "0-1" : "1-0");
    if (board_stalemate(&self->p)) return PyUnicode_FromString("1/2-1/2");
    if (board_insufficient_material(&self->p)) return PyUnicode_FromString("1/2-1/2");
    if (board_is_repetition(self, 5)) return PyUnicode_FromString("1/2-1/2");
    if (self->p.halfmove >= 150) return PyUnicode_FromString("1/2-1/2");
    if (claim_draw && (self->p.halfmove >= 100 || board_is_repetition(self, 3)))
        return PyUnicode_FromString("1/2-1/2");
    return PyUnicode_FromString("*");
}

/* --- FEN --- */

static Py_ssize_t board_fen_str(const Pos *p, char *buf)
{
    Py_ssize_t n = 0;
    int rank, file;

    for (rank = 7; rank >= 0; rank--) {
        int empty = 0;
        for (file = 0; file < 8; file++) {
            int sq = rank * 8 + file;
            int pt = piece_type_at(p, sq);
            if (pt) {
                char c = PIECE_CHARS[pt];
                if (empty) { buf[n++] = (char)('0' + empty); empty = 0; }
                buf[n++] = (p->white & BB(sq)) ? (char)(c - 'a' + 'A') : c;
            } else {
                empty++;
            }
        }
        if (empty) buf[n++] = (char)('0' + empty);
        if (rank) buf[n++] = '/';
    }
    return n;
}

static PyObject *Board_board_fen(BoardObject *self, PyObject *Py_UNUSED(i))
{
    char buf[80];
    Py_ssize_t n = board_fen_str(&self->p, buf);
    return PyUnicode_FromStringAndSize(buf, n);
}

static PyObject *Board_fen(BoardObject *self, PyObject *Py_UNUSED(i))
{
    const Pos *p = &self->p;
    char buf[128];
    Py_ssize_t n = board_fen_str(p, buf);

    buf[n++] = ' ';
    buf[n++] = p->turn ? 'w' : 'b';
    buf[n++] = ' ';
    if (p->castling & CR_WK) buf[n++] = 'K';
    if (p->castling & CR_WQ) buf[n++] = 'Q';
    if (p->castling & CR_BK) buf[n++] = 'k';
    if (p->castling & CR_BQ) buf[n++] = 'q';
    if (!(p->castling & CR_ALL)) buf[n++] = '-';
    buf[n++] = ' ';
    if (p->ep >= 0) {
        buf[n++] = SQ_NAMES[p->ep][0];
        buf[n++] = SQ_NAMES[p->ep][1];
    } else {
        buf[n++] = '-';
    }
    n += PyOS_snprintf(buf + n, sizeof(buf) - (size_t)n, " %d %d",
                       p->halfmove, p->fullmove);
    return PyUnicode_FromStringAndSize(buf, n);
}

typedef struct { const char *s; Py_ssize_t len; } Token;

/* PyErr_Format has no "%.*s". */
static PyObject *substr_obj(const char *s, Py_ssize_t n)
{
    return PyUnicode_DecodeUTF8(s, n, "replace");
}

static int board_set_fen(BoardObject *self, PyObject *fen_obj)
{
    const char *s;
    Py_ssize_t len, i, ntok = 0;
    Token tok[8];
    Pos np;
    int rank_index, turn, castling = 0, ep = -1;
    long halfmove = 0, fullmove = 1;
    struct { int sq; int pt; int color; } place[64];
    int nplace = 0;
    const char *rank_start;
    Py_ssize_t p0len;
    const char *p0;
    int nranks = 0;

    if (!PyUnicode_Check(fen_obj)) {
        PyErr_Format(PyExc_TypeError, "FEN must be a string, not %.200s",
                     Py_TYPE(fen_obj)->tp_name);
        return -1;
    }
    s = PyUnicode_AsUTF8AndSize(fen_obj, &len);
    if (!s) return -1;

    for (i = 0; i < len && ntok < 8; ) {
        while (i < len && (unsigned char)s[i] <= ' ') i++;
        if (i >= len) break;
        tok[ntok].s = s + i;
        while (i < len && (unsigned char)s[i] > ' ') i++;
        tok[ntok].len = (s + i) - tok[ntok].s;
        ntok++;
    }
    if (ntok == 0) {
        PyErr_SetString(PyExc_ValueError, "Invalid FEN: empty string");
        return -1;
    }

    p0 = tok[0].s;
    p0len = tok[0].len;
    for (i = 0; i <= p0len; i++)
        if (i == p0len || p0[i] == '/') nranks++;
    if (nranks != 8) {
        PyErr_Format(PyExc_ValueError,
                     "Invalid FEN: expected 8 ranks, got %d", nranks);
        return -1;
    }

    rank_start = p0;
    for (rank_index = 0; rank_index < 8; rank_index++) {
        const char *end = rank_start;
        int file_index = 0;
        const char *c;

        while (end < p0 + p0len && *end != '/') end++;
        for (c = rank_start; c < end; c++) {
            if (*c >= '0' && *c <= '9') {
                if (*c == '0') {
                    PyErr_Format(PyExc_ValueError,
                        "Invalid FEN: '0' is not a valid skip in rank %d",
                        8 - rank_index);
                    return -1;
                }
                file_index += *c - '0';
            } else {
                char lower = (*c >= 'A' && *c <= 'Z') ? (char)(*c - 'A' + 'a') : *c;
                int pt = symbol_to_piece_type(lower);
                if (file_index > 7) {
                    PyObject *rs = substr_obj(rank_start, end - rank_start);
                    if (rs) {
                        PyErr_Format(PyExc_ValueError,
                            "Invalid FEN: rank %d ('%U') overflows the board",
                            8 - rank_index, rs);
                        Py_DECREF(rs);
                    }
                    return -1;
                }
                if (!pt) {
                    PyErr_Format(PyExc_ValueError,
                                 "Invalid piece symbol: %c", *c);
                    return -1;
                }
                place[nplace].sq = (7 - rank_index) * 8 + file_index;
                place[nplace].pt = pt;
                place[nplace].color = (*c >= 'A' && *c <= 'Z');
                nplace++;
                file_index++;
            }
        }
        if (file_index != 8) {
            PyObject *rs = substr_obj(rank_start, end - rank_start);
            if (rs) {
                PyErr_Format(PyExc_ValueError,
                    "Invalid FEN: rank %d ('%U') describes %d squares, expected 8",
                    8 - rank_index, rs, file_index);
                Py_DECREF(rs);
            }
            return -1;
        }
        rank_start = end + 1;
    }

    if (ntok >= 2 && !(tok[1].len == 1 && (tok[1].s[0] == 'w' || tok[1].s[0] == 'b'))) {
        PyObject *ts = substr_obj(tok[1].s, tok[1].len);
        if (ts) {
            PyErr_Format(PyExc_ValueError,
                "Invalid FEN: side to move must be 'w' or 'b', got '%U'", ts);
            Py_DECREF(ts);
        }
        return -1;
    }
    turn = (ntok < 2 || tok[1].s[0] == 'w');

    if (ntok >= 3 && !(tok[2].len == 1 && tok[2].s[0] == '-')) {
        for (i = 0; i < tok[2].len; i++) {
            switch (tok[2].s[i]) {
                case 'K': castling |= CR_WK; break;
                case 'Q': castling |= CR_WQ; break;
                case 'k': castling |= CR_BK; break;
                case 'q': castling |= CR_BQ; break;
                default: {
                    PyObject *ts = substr_obj(tok[2].s, tok[2].len);
                    if (ts) {
                        PyErr_Format(PyExc_ValueError,
                            "Invalid FEN: bad castling field '%U'", ts);
                        Py_DECREF(ts);
                    }
                    return -1;
                }
            }
        }
    }

    if (ntok >= 4 && !(tok[3].len == 1 && tok[3].s[0] == '-')) {
        if (tok[3].len != 2 ||
            !strchr(FILE_CHARS, tok[3].s[0]) || !strchr(RANK_CHARS, tok[3].s[1])) {
            PyObject *ts = substr_obj(tok[3].s, tok[3].len);
            if (ts) {
                PyErr_Format(PyExc_ValueError,
                    "Invalid FEN: bad en passant square '%U'", ts);
                Py_DECREF(ts);
            }
            return -1;
        }
        ep = (tok[3].s[0] - 'a') + (tok[3].s[1] - '1') * 8;
    }

    for (i = 4; i <= 5 && i < ntok; i++) {
        char numbuf[32];
        char *endp;
        long v;
        if (tok[i].len == 0 || tok[i].len >= (Py_ssize_t)sizeof(numbuf)) goto bad_counter;
        memcpy(numbuf, tok[i].s, (size_t)tok[i].len);
        numbuf[tok[i].len] = '\0';
        v = strtol(numbuf, &endp, 10);
        if (*endp != '\0') goto bad_counter;
        if (i == 4) halfmove = v; else fullmove = v;
    }
    if (halfmove < 0) {
        PyErr_Format(PyExc_ValueError,
                     "Invalid FEN: negative halfmove clock %ld", halfmove);
        return -1;
    }
    if (fullmove < 1) {
        PyErr_Format(PyExc_ValueError,
                     "Invalid FEN: fullmove number must be >= 1, got %ld", fullmove);
        return -1;
    }

    pos_clear(&np);
    np.castling = 0;
    for (i = 0; i < nplace; i++)
        set_piece(&np, place[i].sq, place[i].pt, place[i].color);
    np.turn = turn;
    np.castling = castling;
    np.ep = ep;
    np.halfmove = (int)halfmove;
    np.fullmove = (int)fullmove;

    self->p = np;
    self->stack_len = 0;
    return 0;

bad_counter:
    {
        PyObject *lst = PyList_New(0);
        Py_ssize_t k;
        if (lst) {
            for (k = 4; k < ntok && k < 6; k++) {
                PyObject *item = PyUnicode_FromStringAndSize(tok[k].s, tok[k].len);
                if (!item) break;
                PyList_Append(lst, item);
                Py_DECREF(item);
            }
            PyErr_Format(PyExc_ValueError,
                "Invalid FEN: move counters must be integers, got %R", lst);
            Py_DECREF(lst);
        }
        return -1;
    }
}

static PyObject *Board_set_fen(BoardObject *self, PyObject *arg)
{
    if (board_set_fen(self, arg) < 0) return NULL;
    Py_RETURN_NONE;
}

/* --- copies and rendering --- */

static BoardObject *board_alloc_copy(const BoardObject *src, int with_stack)
{
    BoardObject *b = (BoardObject *)BoardType.tp_alloc(&BoardType, 0);
    if (!b) return NULL;
    b->p = src->p;
    b->stack = NULL;
    b->stack_len = 0;
    b->stack_cap = 0;
    if (with_stack && src->stack_len > 0) {
        b->stack = (Undo *)PyMem_Malloc(sizeof(Undo) * (size_t)src->stack_len);
        if (!b->stack) { Py_DECREF(b); PyErr_NoMemory(); return NULL; }
        memcpy(b->stack, src->stack, sizeof(Undo) * (size_t)src->stack_len);
        b->stack_len = src->stack_len;
        b->stack_cap = src->stack_len;
    }
    return b;
}

static PyObject *Board_copy(BoardObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = { "stack", NULL };
    PyObject *stack = Py_True;
    int with_stack;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O", kwlist, &stack))
        return NULL;
    with_stack = PyObject_IsTrue(stack);
    if (with_stack < 0) return NULL;
    return (PyObject *)board_alloc_copy(self, with_stack);
}

static PyObject *Board_copy_dunder(BoardObject *self, PyObject *Py_UNUSED(i))
{
    return (PyObject *)board_alloc_copy(self, 1);
}

static PyObject *Board_root(BoardObject *self, PyObject *Py_UNUSED(i))
{
    BoardObject *b = board_alloc_copy(self, 0);
    if (!b) return NULL;
    if (self->stack_len > 0) b->p = self->stack[0].pos;
    return (PyObject *)b;
}

static PyObject *Board_str(BoardObject *self)
{
    char buf[256];
    Py_ssize_t n = 0;
    int rank, file;

    for (rank = 7; rank >= 0; rank--) {
        buf[n++] = (char)('1' + rank);
        buf[n++] = ' ';
        for (file = 0; file < 8; file++) {
            int sq = rank * 8 + file;
            int pt = piece_type_at(&self->p, sq);
            if (pt) {
                char c = PIECE_CHARS[pt];
                buf[n++] = (self->p.white & BB(sq)) ? (char)(c - 'a' + 'A') : c;
            } else {
                buf[n++] = '.';
            }
            if (file < 7) buf[n++] = ' ';
        }
        buf[n++] = '\n';
    }
    memcpy(buf + n, "  a b c d e f g h", 17);
    n += 17;
    return PyUnicode_FromStringAndSize(buf, n);
}

static PyObject *Board_repr(BoardObject *self)
{
    PyObject *fen = Board_fen(self, NULL);
    PyObject *r;
    if (!fen) return NULL;
    r = PyUnicode_FromFormat("Board('%U')", fen);
    Py_DECREF(fen);
    return r;
}

static PyObject *Board_unicode(BoardObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = { "invert_color", "empty_square", NULL };
    PyObject *invert = Py_False, *empty = NULL;
    char buf[1024];
    Py_ssize_t n = 0;
    int rank, file, inv;
    static const char *DARK = "\xE2\xAC\x9B";
    static const char *LIGHT = "\xE2\xAC\x9C";
    static const char *FILE_ROW = "  \xEF\xBD\x81\xEF\xBD\x82\xEF\xBD\x83\xEF\xBD\x84"
                                  "\xEF\xBD\x85\xEF\xBD\x86\xEF\xBD\x87\xEF\xBD\x88";

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OO", kwlist, &invert, &empty))
        return NULL;
    inv = PyObject_IsTrue(invert);
    if (inv < 0) return NULL;

    for (rank = 7; rank >= 0; rank--) {
        buf[n++] = (char)('1' + rank);
        buf[n++] = ' ';
        for (file = 0; file < 8; file++) {
            int sq = rank * 8 + file;
            int pt = piece_type_at(&self->p, sq);
            const char *g;
            if (pt) {
                g = UNICODE_PIECES[(self->p.white & BB(sq)) ? 1 : 0][pt];
            } else if (((rank + file) & 1) == 0) {
                g = inv ? LIGHT : DARK;
            } else {
                g = inv ? DARK : LIGHT;
            }
            memcpy(buf + n, g, strlen(g));
            n += (Py_ssize_t)strlen(g);
        }
        buf[n++] = '\n';
    }
    memcpy(buf + n, FILE_ROW, strlen(FILE_ROW));
    n += (Py_ssize_t)strlen(FILE_ROW);
    return PyUnicode_DecodeUTF8(buf, n, NULL);
}

/* --- SAN --- */

static const char *san_check_suffix(Pos *p, int from, int to, int promo)
{
    Pos saved = *p;
    const char *suffix;

    do_move(p, from, to, promo);
    if (board_checkmate(p)) suffix = "#";
    else suffix = board_is_check(p) ? "+" : "";
    *p = saved;
    return suffix;
}

static Py_ssize_t san_disambiguation(const Pos *p, int from, int to, int pt,
                                     char *out)
{
    MoveList ml;
    int from_file = from & 7, from_rank = from >> 3;
    int dominated_file = 0, dominated_rank = 0, i;
    int ks = king_square(p, p->turn);
    Py_ssize_t n = 0;

    ml_init(&ml);
    gen_pseudo(p, &ml);

    for (i = 0; i < ml.n; i++) {
        int of = MV_FROM(ml.v[i]), ot = MV_TO(ml.v[i]);
        if (of == from || ot != to) continue;
        if (piece_type_at(p, of) != pt) continue;
        if (ks < 0 || king_attacked_after(p, of, ot, MV_PROMO(ml.v[i]), ks))
            continue;
        if ((of & 7) == from_file) dominated_file = 1;
        if ((of >> 3) == from_rank) dominated_rank = 1;
    }

    if (!dominated_file && !dominated_rank) {
        for (i = 0; i < ml.n; i++) {
            int of = MV_FROM(ml.v[i]), ot = MV_TO(ml.v[i]);
            if (of == from || ot != to) continue;
            if (piece_type_at(p, of) != pt) continue;
            if (ks >= 0 && !king_attacked_after(p, of, ot, MV_PROMO(ml.v[i]), ks)) {
                out[n++] = FILE_CHARS[from_file];
                ml_free(&ml);
                return n;
            }
        }
        ml_free(&ml);
        return 0;
    }

    ml_free(&ml);
    if (dominated_file && dominated_rank) {
        out[n++] = FILE_CHARS[from_file];
        out[n++] = RANK_CHARS[from_rank];
    } else if (dominated_rank) {
        out[n++] = FILE_CHARS[from_file];
    } else {
        out[n++] = RANK_CHARS[from_rank];
    }
    return n;
}

static PyObject *board_san(BoardObject *self, int from, int to, int promo)
{
    Pos *p = &self->p;
    char buf[16];
    Py_ssize_t n = 0;
    int pt = piece_type_at(p, from);
    int is_capture;
    const char *suffix;

    if (!pt) {
        PyErr_Format(PyExc_ValueError, "No piece at %s", SQ_NAMES[from]);
        return NULL;
    }

    /* Castling is written O-O / O-O-O but still takes a +/# suffix. */
    if (board_is_castling(p, from, to)) {
        if ((to & 7) > (from & 7)) { memcpy(buf, "O-O", 3); n = 3; }
        else { memcpy(buf, "O-O-O", 5); n = 5; }
        suffix = san_check_suffix(p, from, to, promo);
        memcpy(buf + n, suffix, strlen(suffix));
        n += (Py_ssize_t)strlen(suffix);
        return PyUnicode_FromStringAndSize(buf, n);
    }

    is_capture = (pos_occupied(p) & BB(to)) != 0 || board_is_en_passant(p, from, to);

    if (pt == PT_PAWN) {
        if (is_capture) {
            buf[n++] = FILE_CHARS[from & 7];
            buf[n++] = 'x';
        }
        buf[n++] = SQ_NAMES[to][0];
        buf[n++] = SQ_NAMES[to][1];
        if (promo >= 1 && promo <= 6) {
            buf[n++] = '=';
            buf[n++] = (char)(PIECE_CHARS[promo] - 'a' + 'A');
        }
    } else {
        buf[n++] = (char)(PIECE_CHARS[pt] - 'a' + 'A');
        n += san_disambiguation(p, from, to, pt, buf + n);
        if (is_capture) buf[n++] = 'x';
        buf[n++] = SQ_NAMES[to][0];
        buf[n++] = SQ_NAMES[to][1];
    }

    suffix = san_check_suffix(p, from, to, promo);
    memcpy(buf + n, suffix, strlen(suffix));
    n += (Py_ssize_t)strlen(suffix);
    return PyUnicode_FromStringAndSize(buf, n);
}

static PyObject *Board_san(BoardObject *self, PyObject *arg)
{
    int from, to, promo;
    if (get_move_fields(arg, &from, &to, &promo) < 0) return NULL;
    if (move_in_range(from, to) < 0) return NULL;
    return board_san(self, from, to, promo);
}

static PyObject *Board_variation_san(BoardObject *self, PyObject *arg)
{
    PyObject *seq = PySequence_Fast(arg, "variation_san() expects a sequence of moves");
    PyObject *parts, *sep, *joined;
    Py_ssize_t i, len;
    BoardObject *tmp;

    if (!seq) return NULL;
    len = PySequence_Fast_GET_SIZE(seq);
    tmp = board_alloc_copy(self, 0);
    if (!tmp) { Py_DECREF(seq); return NULL; }
    parts = PyList_New(0);
    if (!parts) { Py_DECREF(seq); Py_DECREF(tmp); return NULL; }

    for (i = 0; i < len; i++) {
        PyObject *item = PySequence_Fast_GET_ITEM(seq, i);
        PyObject *s;
        int from, to, promo;
        if (get_move_fields(item, &from, &to, &promo) < 0) goto error;
        if (move_in_range(from, to) < 0) goto error;
        s = board_san(tmp, from, to, promo);
        if (!s) goto error;
        if (PyList_Append(parts, s) < 0) { Py_DECREF(s); goto error; }
        Py_DECREF(s);
        do_move(&tmp->p, from, to, promo);
    }

    sep = PyUnicode_FromString(" ");
    if (!sep) goto error;
    joined = PyUnicode_Join(sep, parts);
    Py_DECREF(sep);
    Py_DECREF(parts);
    Py_DECREF(tmp);
    Py_DECREF(seq);
    return joined;

error:
    Py_DECREF(parts);
    Py_DECREF(tmp);
    Py_DECREF(seq);
    return NULL;
}

/* --- SAN parsing --- */

static int is_ascii_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static PyObject *repr_of(const char *s, Py_ssize_t n)
{
    PyObject *u = PyUnicode_FromStringAndSize(s, n);
    PyObject *r;
    if (!u) return NULL;
    r = PyObject_Repr(u);
    Py_DECREF(u);
    return r;
}

static PyObject *Board_parse_san(BoardObject *self, PyObject *arg)
{
    const char *src;
    Py_ssize_t srclen, i, n;
    char stackbuf[256];
    char *buf;
    int heap = 0;
    int promotion = 0, piece_type = PT_PAWN;
    int to_square, from_file = -1, from_rank = -1;
    MoveList ml;
    int matches[8], nmatch = 0, k;
    PyObject *result = NULL;

    if (!PyUnicode_Check(arg)) {
        PyErr_Format(InvalidMoveError, "Invalid SAN: %R has no move text", arg);
        return NULL;
    }
    src = PyUnicode_AsUTF8AndSize(arg, &srclen);
    if (!src) return NULL;

    if (srclen + 1 > (Py_ssize_t)sizeof(stackbuf)) {
        buf = (char *)PyMem_Malloc((size_t)srclen + 1);
        if (!buf) return PyErr_NoMemory();
        heap = 1;
    } else {
        buf = stackbuf;
    }
    memcpy(buf, src, (size_t)srclen);
    n = srclen;

    /* strip() */
    {
        Py_ssize_t a = 0;
        while (a < n && is_ascii_space(buf[a])) a++;
        while (n > a && is_ascii_space(buf[n - 1])) n--;
        if (a) { memmove(buf, buf + a, (size_t)(n - a)); n -= a; }
    }

    /* Strip NAGs, "e.p.", glyphs, then check marks. */
    {
        Py_ssize_t w = 0;
        for (i = 0; i < n; ) {
            if (buf[i] == '$' && i + 1 < n && buf[i + 1] >= '0' && buf[i + 1] <= '9') {
                i++;
                while (i < n && buf[i] >= '0' && buf[i] <= '9') i++;
            } else {
                buf[w++] = buf[i++];
            }
        }
        n = w;
    }
    {
        Py_ssize_t j = n;
        while (j > 0 && is_ascii_space(buf[j - 1])) j--;
        if (j > 0 && buf[j - 1] == '.') j--;
        if (j > 0 && (buf[j - 1] == 'p' || buf[j - 1] == 'P')) {
            j--;
            if (j > 0 && buf[j - 1] == '.') j--;
            if (j > 0 && (buf[j - 1] == 'e' || buf[j - 1] == 'E')) {
                j--;
                while (j > 0 && is_ascii_space(buf[j - 1])) j--;
                n = j;
            }
        }
    }
    {
        Py_ssize_t w = 0;
        for (i = 0; i < n; i++)
            if (buf[i] != '!' && buf[i] != '?') buf[w++] = buf[i];
        n = w;
    }
    while (n > 0 && (buf[n - 1] == '+' || buf[n - 1] == '#')) n--;
    {
        Py_ssize_t a = 0;
        while (a < n && is_ascii_space(buf[a])) a++;
        while (n > a && is_ascii_space(buf[n - 1])) n--;
        if (a) { memmove(buf, buf + a, (size_t)(n - a)); n -= a; }
    }

    if (n == 0) {
        PyErr_Format(InvalidMoveError, "Invalid SAN: %R has no move text", arg);
        goto done;
    }

    if ((n == 3 && (memcmp(buf, "O-O", 3) == 0 || memcmp(buf, "0-0", 3) == 0)) ||
        (n == 5 && (memcmp(buf, "O-O-O", 5) == 0 || memcmp(buf, "0-0-0", 5) == 0))) {
        int ks = king_square(&self->p, self->p.turn);
        int to;
        if (ks < 0) {
            PyErr_SetString(PyExc_ValueError, "No king on board");
            goto done;
        }
        if (n == 3) to = self->p.turn ? SQ_G1 : SQ_G8;
        else to = self->p.turn ? SQ_C1 : SQ_C8;
        result = move_new_raw(ks, to, 0);
        goto done;
    }

    /* promotion */
    for (i = 0; i < n; i++) {
        if (buf[i] == '=') {
            Py_ssize_t a = i + 1, b = n;
            char c;
            while (a < b && is_ascii_space(buf[a])) a++;
            while (b > a && is_ascii_space(buf[b - 1])) b--;
            c = (b - a == 1) ? buf[a] : '\0';
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            promotion = c ? symbol_to_piece_type(c) : 0;
            if (!promotion) {
                PyObject *pr = repr_of(buf + i + 1, n - i - 1);
                if (pr) {
                    PyErr_Format(InvalidMoveError,
                        "Invalid promotion piece in SAN %R: %U", arg, pr);
                    Py_DECREF(pr);
                }
                goto done;
            }
            n = i;
            break;
        }
    }

    {
        Py_ssize_t w = 0;
        for (i = 0; i < n; i++)
            if (buf[i] != 'x') buf[w++] = buf[i];
        n = w;
    }

    if (n == 0) {
        PyErr_Format(InvalidMoveError, "Invalid SAN: %R has no move text", arg);
        goto done;
    }

    if (buf[0] >= 'A' && buf[0] <= 'Z') {
        piece_type = symbol_to_piece_type((char)(buf[0] - 'A' + 'a'));
        if (!piece_type) {
            PyObject *pr = repr_of(buf, 1);
            if (pr) {
                PyErr_Format(InvalidMoveError,
                             "Invalid piece in SAN %R: %U", arg, pr);
                Py_DECREF(pr);
            }
            goto done;
        }
        memmove(buf, buf + 1, (size_t)(n - 1));
        n--;
    }

    if (n < 2) {
        PyErr_Format(InvalidMoveError, "Invalid SAN %R: missing target square", arg);
        goto done;
    }
    if (!strchr(FILE_CHARS, buf[n - 2]) || !buf[n - 2] ||
        !strchr(RANK_CHARS, buf[n - 1]) || !buf[n - 1]) {
        PyObject *pr = repr_of(buf + n - 2, 2);
        if (pr) {
            PyErr_Format(InvalidMoveError,
                         "Invalid SAN %R: bad target square %U", arg, pr);
            Py_DECREF(pr);
        }
        goto done;
    }
    to_square = (buf[n - 2] - 'a') + (buf[n - 1] - '1') * 8;
    n -= 2;

    for (i = 0; i < n; i++) {
        if (buf[i] >= 'a' && buf[i] <= 'h') from_file = buf[i] - 'a';
        else if (buf[i] >= '1' && buf[i] <= '8') from_rank = buf[i] - '1';
    }

    ml_init(&ml);
    gen_legal(&self->p, &ml);
    for (k = 0; k < ml.n; k++) {
        int m = ml.v[k];
        int from = MV_FROM(m);
        if (MV_TO(m) != to_square) continue;
        if (MV_PROMO(m) != promotion) continue;
        if (piece_type_at(&self->p, from) != piece_type) continue;
        if (from_file >= 0 && (from & 7) != from_file) continue;
        if (from_rank >= 0 && (from >> 3) != from_rank) continue;
        if (nmatch < 8) matches[nmatch] = m;
        nmatch++;
    }
    ml_free(&ml);

    if (nmatch == 0) {
        PyObject *fen = Board_fen(self, NULL);
        if (fen) {
            PyObject *fr = PyObject_Repr(fen);
            if (fr) {
                PyErr_Format(IllegalMoveError,
                    "Illegal or unparseable SAN %R in position %U", arg, fr);
                Py_DECREF(fr);
            }
            Py_DECREF(fen);
        }
        goto done;
    }
    if (nmatch > 1) {
        PyObject *names = PyList_New(0);
        int lim = nmatch < 8 ? nmatch : 8;
        if (names) {
            PyObject *sep, *joined;
            for (k = 0; k < lim; k++) {
                char ubuf[8];
                MoveObject tmp;
                Py_ssize_t ulen;
                PyObject *u;
                tmp.from_square = MV_FROM(matches[k]);
                tmp.to_square = MV_TO(matches[k]);
                tmp.promotion = MV_PROMO(matches[k]);
                ulen = move_uci_str(&tmp, ubuf);
                if (ulen < 0) break;
                u = PyUnicode_FromStringAndSize(ubuf, ulen);
                if (!u) break;
                PyList_Append(names, u);
                Py_DECREF(u);
            }
            sep = PyUnicode_FromString(", ");
            joined = sep ? PyUnicode_Join(sep, names) : NULL;
            Py_XDECREF(sep);
            Py_DECREF(names);
            if (joined) {
                PyErr_Format(AmbiguousMoveError,
                             "Ambiguous SAN %R: matches %U", arg, joined);
                Py_DECREF(joined);
            }
        }
        goto done;
    }

    result = move_new_raw(MV_FROM(matches[0]), MV_TO(matches[0]),
                          MV_PROMO(matches[0]));

done:
    if (heap) PyMem_Free(buf);
    return result;
}

static PyObject *Board_push_san(BoardObject *self, PyObject *arg)
{
    PyObject *move = Board_parse_san(self, arg);
    PyObject *r;

    if (!move) return NULL;
    r = Board_push(self, move);
    if (!r) { Py_DECREF(move); return NULL; }
    Py_DECREF(r);
    return move;
}

/* --- generators --- */

static PyObject *Board_gen_pseudo(BoardObject *self, PyObject *Py_UNUSED(i))
{
    MoveList ml;
    ml_init(&ml);
    gen_pseudo(&self->p, &ml);
    return moveiter_from_list(&ml);
}

static PyObject *Board_gen_legal(BoardObject *self, PyObject *Py_UNUSED(i))
{
    MoveList ml;
    ml_init(&ml);
    gen_legal(&self->p, &ml);
    return moveiter_from_list(&ml);
}

/* LegalMoveGenerator */

static void LMG_dealloc(LMGObject *self)
{
    Py_XDECREF(self->board);
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *LMG_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = { "board", "pseudo", NULL };
    PyObject *board, *pseudo = Py_False;
    LMGObject *self;
    int p;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!|O", kwlist,
                                     &BoardType, &board, &pseudo))
        return NULL;
    p = PyObject_IsTrue(pseudo);
    if (p < 0) return NULL;
    self = (LMGObject *)type->tp_alloc(type, 0);
    if (!self) return NULL;
    Py_INCREF(board);
    self->board = (BoardObject *)board;
    self->pseudo = p;
    return (PyObject *)self;
}

static void lmg_generate(LMGObject *self, MoveList *ml)
{
    if (self->pseudo) gen_pseudo(&self->board->p, ml);
    else gen_legal(&self->board->p, ml);
}

static PyObject *LMG_iter(LMGObject *self)
{
    if (self->pseudo) return Board_gen_pseudo(self->board, NULL);
    return Board_gen_legal(self->board, NULL);
}

static Py_ssize_t LMG_len(LMGObject *self)
{
    if (self->pseudo) {
        MoveList ml;
        Py_ssize_t n;
        ml_init(&ml);
        gen_pseudo(&self->board->p, &ml);
        n = ml.n;
        ml_free(&ml);
        return n;
    }
    return count_legal(&self->board->p, 0);
}

static int LMG_bool(LMGObject *self)
{
    if (self->pseudo) return LMG_len(self) != 0;
    return count_legal(&self->board->p, 1) != 0;
}

static int LMG_contains(LMGObject *self, PyObject *item)
{
    MoveList ml;
    int from, to, promo, i, found = 0;

    if (get_move_fields(item, &from, &to, &promo) < 0) {
        PyErr_Clear();
        return 0;
    }
    if (from < 0 || from > 63 || to < 0 || to > 63) return 0;

    ml_init(&ml);
    lmg_generate(self, &ml);
    for (i = 0; i < ml.n; i++)
        if (ml.v[i] == MV(from, to, promo)) { found = 1; break; }
    ml_free(&ml);
    return found;
}

static PyObject *LMG_count(LMGObject *self, PyObject *Py_UNUSED(i))
{
    return PyLong_FromSsize_t(LMG_len(self));
}

static PyObject *LMG_get_board(LMGObject *self, void *c)
{
    Py_INCREF(self->board);
    return (PyObject *)self->board;
}

static PyObject *LMG_repr(LMGObject *self)
{
    MoveList ml;
    PyObject *parts, *sep, *joined, *out;
    int i;

    ml_init(&ml);
    lmg_generate(self, &ml);
    parts = PyList_New(0);
    if (!parts) { ml_free(&ml); return NULL; }
    for (i = 0; i < ml.n; i++) {
        char ubuf[8];
        MoveObject tmp;
        Py_ssize_t ulen;
        PyObject *u;
        tmp.from_square = MV_FROM(ml.v[i]);
        tmp.to_square = MV_TO(ml.v[i]);
        tmp.promotion = MV_PROMO(ml.v[i]);
        ulen = move_uci_str(&tmp, ubuf);
        if (ulen < 0) { Py_DECREF(parts); ml_free(&ml); return NULL; }
        u = PyUnicode_FromStringAndSize(ubuf, ulen);
        if (!u) { Py_DECREF(parts); ml_free(&ml); return NULL; }
        PyList_Append(parts, u);
        Py_DECREF(u);
    }
    ml_free(&ml);
    sep = PyUnicode_FromString(", ");
    joined = sep ? PyUnicode_Join(sep, parts) : NULL;
    Py_XDECREF(sep);
    Py_DECREF(parts);
    if (!joined) return NULL;
    out = PyUnicode_FromFormat(self->pseudo ? "<PseudoLegalMoveGenerator (%U)>"
                                            : "<LegalMoveGenerator (%U)>", joined);
    Py_DECREF(joined);
    return out;
}

static PyMethodDef LMG_methods[] = {
    { "count", (PyCFunction)LMG_count, METH_NOARGS, "Number of legal moves." },
    { NULL }
};

static PyGetSetDef LMG_getset[] = {
    { "_board", (getter)LMG_get_board, NULL, "The board being generated from.", NULL },
    { NULL }
};

static PySequenceMethods LMG_as_sequence = { 0 };
static PyNumberMethods LMG_as_number = { 0 };

static PyTypeObject LMGType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bmm_chess._core.LegalMoveGenerator",
    sizeof(LMGObject),
};

/* position inspection */

#define BB_ALL 0xFFFFFFFFFFFFFFFFULL
#define BB_BACKRANKS (RANK_1_BB | RANK_8_BB)
#define BB_LIGHT_SQUARES 0x55AA55AA55AA55AAULL
#define BB_DARK_SQUARES  0xAA55AA55AA55AA55ULL

static PyObject *Board_attacks(BoardObject *self, PyObject *arg)
{
    PyObject *mask = Board_attacks_mask(self, arg);
    PyObject *ss;
    if (!mask) return NULL;
    ss = squareset_new_raw(PyLong_AsUnsignedLongLongMask(mask));
    Py_DECREF(mask);
    return ss;
}

/* The pin line, or every square when not pinned. */
static U64 pin_mask_of(const Pos *p, int color, int sq)
{
    int king = king_square(p, color);
    U64 sqm = BB(sq), rays, sliders, snipers, their, occ;

    if (king < 0) return BB_ALL;
    their = color ? p->black : p->white;
    occ = pos_occupied(p);

    if (ROOK_RAYS[king] & sqm) {
        rays = ROOK_RAYS[king];
        sliders = p->rooks | p->queens;
    } else if (BISHOP_RAYS[king] & sqm) {
        rays = BISHOP_RAYS[king];
        sliders = p->bishops | p->queens;
    } else {
        return BB_ALL;
    }

    snipers = rays & sliders & their;
    while (snipers) {
        int sniper = pop_lsb(&snipers);
        if ((BETWEEN[sniper][king] & (occ | sqm)) == sqm)
            return RAY[king][sniper];
    }
    return BB_ALL;
}

static PyObject *Board_pin_mask(BoardObject *self, PyObject *args)
{
    PyObject *color_obj;
    int sq, c;
    if (!PyArg_ParseTuple(args, "Oi", &color_obj, &sq)) return NULL;
    if (check_square_range(sq) < 0) return NULL;
    c = arg_color(color_obj);
    if (c < 0) return NULL;
    return PyLong_FromUnsignedLongLong(pin_mask_of(&self->p, c, sq));
}

static PyObject *Board_pin(BoardObject *self, PyObject *args)
{
    PyObject *color_obj;
    int sq, c;
    if (!PyArg_ParseTuple(args, "Oi", &color_obj, &sq)) return NULL;
    if (check_square_range(sq) < 0) return NULL;
    c = arg_color(color_obj);
    if (c < 0) return NULL;
    return squareset_new_raw(pin_mask_of(&self->p, c, sq));
}

static PyObject *Board_is_pinned(BoardObject *self, PyObject *args)
{
    PyObject *color_obj;
    int sq, c;
    if (!PyArg_ParseTuple(args, "Oi", &color_obj, &sq)) return NULL;
    if (check_square_range(sq) < 0) return NULL;
    c = arg_color(color_obj);
    if (c < 0) return NULL;
    return PyBool_FromLong(pin_mask_of(&self->p, c, sq) != BB_ALL);
}

static PyObject *Board_ply(BoardObject *self, PyObject *Py_UNUSED(i))
{
    return PyLong_FromLong(2 * (self->p.fullmove - 1) + (self->p.turn ? 0 : 1));
}

static PyObject *Board_was_into_check(BoardObject *self, PyObject *Py_UNUSED(i))
{
    int king = king_square(&self->p, !self->p.turn);
    if (king < 0) Py_RETURN_FALSE;
    return PyBool_FromLong(
        square_attacked(&self->p, self->p.turn, king, pos_occupied(&self->p)));
}

static int board_is_zeroing(const Pos *p, int from, int to)
{
    U64 touched = BB(from) ^ BB(to);
    return (touched & p->pawns) != 0 ||
           (touched & (p->turn ? p->black : p->white)) != 0;
}

static PyObject *Board_is_zeroing(BoardObject *self, PyObject *arg)
{
    int from, to, promo;
    if (get_move_fields(arg, &from, &to, &promo) < 0) return NULL;
    if (move_in_range(from, to) < 0) return NULL;
    return PyBool_FromLong(board_is_zeroing(&self->p, from, to));
}

/* Rights as rook squares (python-chess form), from our flag bits. */
static U64 clean_castling_rooks(const Pos *p)
{
    U64 clean = 0;
    int c, k;
    for (c = 0; c < 2; c++) {
        U64 ours = c ? p->white : p->black;
        if (!(p->kings & ours & BB(CASTLE_KING_FROM[c]))) continue;
        for (k = 0; k < 2; k++) {
            int rook = CASTLE_ROOK_FROM[c][k];
            if (!(p->castling & CASTLE_RIGHT[c][k])) continue;
            if (p->rooks & ours & BB(rook)) clean |= BB(rook);
        }
    }
    return clean;
}

static int reduces_castling_rights(const Pos *p, int from, int to)
{
    U64 cr = clean_castling_rooks(p);
    U64 touched = BB(from) ^ BB(to);
    if (touched & cr) return 1;
    if ((cr & RANK_1_BB) && (touched & p->kings & p->white)) return 1;
    if ((cr & RANK_8_BB) && (touched & p->kings & p->black)) return 1;
    return 0;
}

static int board_has_pseudo_legal_ep(const Pos *p)
{
    MoveList ml;
    int i, found = 0;
    if (p->ep < 0) return 0;
    ml_init(&ml);
    gen_pawn_moves(p, &ml);
    for (i = 0; i < ml.n; i++) {
        int m = ml.v[i];
        if (MV_TO(m) == p->ep && board_is_en_passant(p, MV_FROM(m), MV_TO(m))) {
            found = 1;
            break;
        }
    }
    ml_free(&ml);
    return found;
}

static int board_has_legal_ep(const Pos *p)
{
    MoveList ml;
    int i, found = 0, king_sq;
    if (p->ep < 0) return 0;
    king_sq = king_square(p, p->turn);
    if (king_sq < 0) return 0;
    ml_init(&ml);
    gen_pawn_moves(p, &ml);
    for (i = 0; i < ml.n; i++) {
        int m = ml.v[i];
        if (MV_TO(m) != p->ep) continue;
        if (!board_is_en_passant(p, MV_FROM(m), MV_TO(m))) continue;
        if (!king_attacked_after(p, MV_FROM(m), MV_TO(m), MV_PROMO(m), king_sq)) {
            found = 1;
            break;
        }
    }
    ml_free(&ml);
    return found;
}

static PyObject *Board_has_pseudo_legal_en_passant(BoardObject *self, PyObject *Py_UNUSED(i))
{
    return PyBool_FromLong(board_has_pseudo_legal_ep(&self->p));
}

static PyObject *Board_has_legal_en_passant(BoardObject *self, PyObject *Py_UNUSED(i))
{
    return PyBool_FromLong(board_has_legal_ep(&self->p));
}

static PyObject *Board_is_irreversible(BoardObject *self, PyObject *arg)
{
    int from, to, promo;
    if (get_move_fields(arg, &from, &to, &promo) < 0) return NULL;
    if (move_in_range(from, to) < 0) return NULL;
    return PyBool_FromLong(board_is_zeroing(&self->p, from, to) ||
                           reduces_castling_rights(&self->p, from, to) ||
                           board_has_legal_ep(&self->p));
}

static PyObject *Board_has_insufficient_material(BoardObject *self, PyObject *arg)
{
    const Pos *p = &self->p;
    U64 ours, theirs;
    int c = arg_color(arg);

    if (c < 0) return NULL;
    ours = c ? p->white : p->black;
    theirs = c ? p->black : p->white;

    if (ours & (p->pawns | p->rooks | p->queens)) Py_RETURN_FALSE;

    if (ours & p->knights) {
        /* Unless the opponent has material that can be forced to self-block. */
        return PyBool_FromLong(popcount64(ours) <= 2 &&
                               !(theirs & ~p->kings & ~p->queens));
    }

    if (ours & p->bishops) {
        /* All bishops one colour, and no pawn or knight to force a selfmate. */
        int same_color = !(p->bishops & BB_DARK_SQUARES) ||
                         !(p->bishops & BB_LIGHT_SQUARES);
        return PyBool_FromLong(same_color && !p->pawns && !p->knights);
    }

    Py_RETURN_TRUE;
}

static PyObject *Board_is_pseudo_legal(BoardObject *self, PyObject *arg)
{
    MoveList ml;
    int from, to, promo, i, found = 0;

    if (get_move_fields(arg, &from, &to, &promo) < 0) return NULL;
    if (from < 0 || from > 63 || to < 0 || to > 63) Py_RETURN_FALSE;
    if (from == to && promo == 0) Py_RETURN_FALSE;   /* null move */

    ml_init(&ml);
    gen_pseudo(&self->p, &ml);
    for (i = 0; i < ml.n; i++)
        if (ml.v[i] == MV(from, to, promo)) { found = 1; break; }
    ml_free(&ml);
    return PyBool_FromLong(found);
}

/* The en passant square the position could legitimately have, or -1. */
static int valid_ep_square(const Pos *p)
{
    U64 pawn_mask, behind_mask;
    int ep = p->ep;

    if (ep < 0) return -1;
    if (p->turn) {
        if ((ep >> 3) != 5) return -1;
        pawn_mask = BB(ep) >> 8;
        behind_mask = BB(ep) << 8;
    } else {
        if ((ep >> 3) != 2) return -1;
        pawn_mask = BB(ep) << 8;
        behind_mask = BB(ep) >> 8;
    }
    if (!(p->pawns & (p->turn ? p->black : p->white) & pawn_mask)) return -1;
    if (pos_occupied(p) & BB(ep)) return -1;
    if (pos_occupied(p) & behind_mask) return -1;
    return ep;
}

#define ST_NO_WHITE_KING        (1 << 0)
#define ST_NO_BLACK_KING        (1 << 1)
#define ST_TOO_MANY_KINGS       (1 << 2)
#define ST_TOO_MANY_WHITE_PAWNS (1 << 3)
#define ST_TOO_MANY_BLACK_PAWNS (1 << 4)
#define ST_PAWNS_ON_BACKRANK    (1 << 5)
#define ST_TOO_MANY_WHITE_PIECES (1 << 6)
#define ST_TOO_MANY_BLACK_PIECES (1 << 7)
#define ST_BAD_CASTLING_RIGHTS  (1 << 8)
#define ST_INVALID_EP_SQUARE    (1 << 9)
#define ST_OPPOSITE_CHECK       (1 << 10)
#define ST_EMPTY                (1 << 11)
#define ST_TOO_MANY_CHECKERS    (1 << 15)
#define ST_IMPOSSIBLE_CHECK     (1 << 16)

static long board_status(const Pos *p)
{
    long errors = 0;
    U64 occ = pos_occupied(p);
    int king, valid_ep;
    U64 checkers = 0;

    if (!occ) errors |= ST_EMPTY;
    if (!(p->white & p->kings)) errors |= ST_NO_WHITE_KING;
    if (!(p->black & p->kings)) errors |= ST_NO_BLACK_KING;
    if (popcount64(occ & p->kings) > 2) errors |= ST_TOO_MANY_KINGS;

    if (popcount64(p->white) > 16) errors |= ST_TOO_MANY_WHITE_PIECES;
    if (popcount64(p->black) > 16) errors |= ST_TOO_MANY_BLACK_PIECES;
    if (popcount64(p->white & p->pawns) > 8) errors |= ST_TOO_MANY_WHITE_PAWNS;
    if (popcount64(p->black & p->pawns) > 8) errors |= ST_TOO_MANY_BLACK_PAWNS;
    if (p->pawns & BB_BACKRANKS) errors |= ST_PAWNS_ON_BACKRANK;

    /* A right without its king or rook means a hand-written FEN. */
    {
        U64 clean = clean_castling_rooks(p);
        U64 claimed = 0;
        int c, k;
        for (c = 0; c < 2; c++)
            for (k = 0; k < 2; k++)
                if (p->castling & CASTLE_RIGHT[c][k])
                    claimed |= BB(CASTLE_ROOK_FROM[c][k]);
        if (claimed != clean) errors |= ST_BAD_CASTLING_RIGHTS;
    }

    valid_ep = valid_ep_square(p);
    if (p->ep != valid_ep) errors |= ST_INVALID_EP_SQUARE;

    king = king_square(p, !p->turn);
    if (king >= 0 && square_attacked(p, p->turn, king, occ))
        errors |= ST_OPPOSITE_CHECK;

    king = king_square(p, p->turn);
    if (king >= 0) checkers = attackers_to(p, !p->turn, king, occ);

    if (checkers) {
        int n = popcount64(checkers);
        if (n > 2) errors |= ST_TOO_MANY_CHECKERS;

        if (valid_ep >= 0) {
            /* Rewind the double push; only the check it discovered can survive. */
            int pushed_to = valid_ep ^ 8;
            int pushed_from = valid_ep ^ 24;
            U64 before = (occ & ~BB(pushed_to)) | BB(pushed_from);
            if (n > 1 || (msb64(checkers) != pushed_to &&
                          square_attacked(p, !p->turn, king, before)))
                errors |= ST_IMPOSSIBLE_CHECK;
        } else if (n == 2 && (RAY[ctz64(checkers)][msb64(checkers)] & BB(king))) {
            /* Two checkers on a line through the king is impossible. */
            errors |= ST_IMPOSSIBLE_CHECK;
        }
    }

    return errors;
}

static PyObject *Board_status(BoardObject *self, PyObject *Py_UNUSED(i))
{
    return PyLong_FromLong(board_status(&self->p));
}

static PyObject *Board_is_valid(BoardObject *self, PyObject *Py_UNUSED(i))
{
    return PyBool_FromLong(board_status(&self->p) == 0);
}

/* PolyGlot Zobrist hashing */

static U64 polyglot_hash(const Pos *p)
{
    U64 h = 0, occ = pos_occupied(p), clean;

    while (occ) {
        int sq = pop_lsb(&occ);
        int pt = piece_type_at(p, sq);
        int colour = (p->white & BB(sq)) ? 1 : 0;
        h ^= POLYGLOT_KEYS[64 * ((pt - 1) * 2 + colour) + sq];
    }

    /* Unbacked rights are not hashed: books hold real positions. */
    clean = clean_castling_rooks(p);
    if (clean & BB(SQ_H1)) h ^= POLYGLOT_KEYS[768];
    if (clean & BB(SQ_A1)) h ^= POLYGLOT_KEYS[769];
    if (clean & BB(SQ_H8)) h ^= POLYGLOT_KEYS[770];
    if (clean & BB(SQ_A8)) h ^= POLYGLOT_KEYS[771];

    if (p->ep >= 0) {
        /* Only with a pawn alongside; legality is irrelevant. */
        U64 epm = p->turn ? (BB(p->ep) >> 8) : (BB(p->ep) << 8);
        epm = ((epm & ~FILE_A_BB) >> 1) | ((epm & ~FILE_H_BB) << 1);
        if (epm & p->pawns & (p->turn ? p->white : p->black))
            h ^= POLYGLOT_KEYS[772 + (p->ep & 7)];
    }

    if (p->turn) h ^= POLYGLOT_KEYS[780];
    return h;
}

static PyObject *Board_zobrist_hash(BoardObject *self, PyObject *Py_UNUSED(i))
{
    return PyLong_FromUnsignedLongLong(polyglot_hash(&self->p));
}

/* move parsing helpers */

static int board_move_is_legal(const Pos *p, int from, int to, int promo)
{
    MoveList ml;
    int i, found = 0;
    ml_init(&ml);
    gen_legal(p, &ml);
    for (i = 0; i < ml.n; i++)
        if (ml.v[i] == MV(from, to, promo)) { found = 1; break; }
    ml_free(&ml);
    return found;
}

static PyObject *Board_parse_uci(BoardObject *self, PyObject *arg)
{
    PyObject *move = Move_from_uci(&MoveType, arg);
    MoveObject *m;

    if (!move) {
        /* Relabel so callers can tell malformed from illegal. */
        PyObject *type, *value, *tb;
        PyErr_Fetch(&type, &value, &tb);
        PyErr_NormalizeException(&type, &value, &tb);
        PyErr_SetObject(InvalidMoveError, value);
        Py_XDECREF(type); Py_XDECREF(value); Py_XDECREF(tb);
        return NULL;
    }

    m = (MoveObject *)move;
    if (m->from_square == m->to_square && m->promotion == 0)
        return move;   /* null move */

    if (!board_move_is_legal(&self->p, m->from_square, m->to_square, m->promotion)) {
        PyObject *fen = Board_fen(self, NULL);
        if (fen) {
            PyErr_Format(IllegalMoveError, "illegal uci: %R in %U", arg, fen);
            Py_DECREF(fen);
        }
        Py_DECREF(move);
        return NULL;
    }
    return move;
}

static PyObject *Board_find_move(BoardObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = { "from_square", "to_square", "promotion", NULL };
    PyObject *promo_obj = Py_None;
    int from, to, promo = 0;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "ii|O", kwlist,
                                     &from, &to, &promo_obj))
        return NULL;
    if (move_in_range(from, to) < 0) return NULL;

    if (promo_obj == Py_None) {
        /* Back-rank pawn must promote; assume a queen. */
        if ((self->p.pawns & BB(from)) && (BB(to) & BB_BACKRANKS))
            promo = PT_QUEEN;
    } else {
        long v = PyLong_AsLong(promo_obj);
        if (v == -1 && PyErr_Occurred()) return NULL;
        promo = (int)v;
    }

    if (!board_move_is_legal(&self->p, from, to, promo)) {
        PyObject *fen = Board_fen(self, NULL);
        if (fen) {
            PyErr_Format(IllegalMoveError, "no matching legal move for %s%s%s in %U",
                         SQ_NAMES[from], SQ_NAMES[to],
                         promo ? "(promotion)" : "", fen);
            Py_DECREF(fen);
        }
        return NULL;
    }
    return move_new_raw(from, to, promo);
}

static PyObject *Board_lan(BoardObject *self, PyObject *arg)
{
    Pos *p = &self->p;
    char buf[16];
    Py_ssize_t n = 0;
    int from, to, promo, pt;
    const char *suffix;

    if (get_move_fields(arg, &from, &to, &promo) < 0) return NULL;
    if (move_in_range(from, to) < 0) return NULL;

    pt = piece_type_at(p, from);
    if (!pt) {
        PyErr_Format(PyExc_ValueError, "No piece at %s", SQ_NAMES[from]);
        return NULL;
    }

    if (board_is_castling(p, from, to)) {
        if ((to & 7) > (from & 7)) { memcpy(buf, "O-O", 3); n = 3; }
        else { memcpy(buf, "O-O-O", 5); n = 5; }
    } else {
        if (pt != PT_PAWN) buf[n++] = (char)(PIECE_CHARS[pt] - 'a' + 'A');
        buf[n++] = SQ_NAMES[from][0];
        buf[n++] = SQ_NAMES[from][1];
        buf[n++] = ((pos_occupied(p) & BB(to)) || board_is_en_passant(p, from, to))
                   ? 'x' : '-';
        buf[n++] = SQ_NAMES[to][0];
        buf[n++] = SQ_NAMES[to][1];
        if (promo >= 1 && promo <= 6) {
            buf[n++] = '=';
            buf[n++] = (char)(PIECE_CHARS[promo] - 'a' + 'A');
        }
    }

    suffix = san_check_suffix(p, from, to, promo);
    memcpy(buf + n, suffix, strlen(suffix));
    n += (Py_ssize_t)strlen(suffix);
    return PyUnicode_FromStringAndSize(buf, n);
}

static PyObject *Board_san_and_push(BoardObject *self, PyObject *arg)
{
    int from, to, promo;
    PyObject *san, *pushed;

    if (get_move_fields(arg, &from, &to, &promo) < 0) return NULL;
    if (move_in_range(from, to) < 0) return NULL;
    san = board_san(self, from, to, promo);
    if (!san) return NULL;
    pushed = Board_push(self, arg);
    if (!pushed) { Py_DECREF(san); return NULL; }
    Py_DECREF(pushed);
    return san;
}

/* board editing */

static void pos_reset_board(Pos *p)
{
    p->pawns = 0x00FF00000000FF00ULL;
    p->knights = 0x4200000000000042ULL;
    p->bishops = 0x2400000000000024ULL;
    p->rooks = 0x8100000000000081ULL;
    p->queens = 0x0800000000000008ULL;
    p->kings = 0x1000000000000010ULL;
    p->white = 0x000000000000FFFFULL;
    p->black = 0xFFFF000000000000ULL;
}

static PyObject *Board_clear_board(BoardObject *self, PyObject *Py_UNUSED(i))
{
    self->p.pawns = self->p.knights = self->p.bishops = 0;
    self->p.rooks = self->p.queens = self->p.kings = 0;
    self->p.white = self->p.black = 0;
    self->stack_len = 0;
    Py_RETURN_NONE;
}

static PyObject *Board_clear(BoardObject *self, PyObject *Py_UNUSED(i))
{
    pos_clear(&self->p);
    self->p.castling = 0;
    self->stack_len = 0;
    Py_RETURN_NONE;
}

static PyObject *Board_reset_board(BoardObject *self, PyObject *Py_UNUSED(i))
{
    pos_reset_board(&self->p);
    self->stack_len = 0;
    Py_RETURN_NONE;
}

static PyObject *Board_reset(BoardObject *self, PyObject *Py_UNUSED(i))
{
    pos_clear(&self->p);
    pos_reset_board(&self->p);
    self->stack_len = 0;
    Py_RETURN_NONE;
}

static PyObject *Board_empty(PyTypeObject *cls, PyObject *Py_UNUSED(i))
{
    BoardObject *b = (BoardObject *)BoardType.tp_alloc(&BoardType, 0);
    if (!b) return NULL;
    pos_clear(&b->p);
    b->p.castling = 0;
    b->stack = NULL;
    b->stack_len = 0;
    b->stack_cap = 0;
    return (PyObject *)b;
}

static PyObject *Board_set_board_fen(BoardObject *self, PyObject *arg)
{
    PyObject *full, *result;
    if (!PyUnicode_Check(arg)) {
        PyErr_Format(PyExc_TypeError, "board FEN must be a string, not %.200s",
                     Py_TYPE(arg)->tp_name);
        return NULL;
    }
    full = PyUnicode_FromFormat("%U w - - 0 1", arg);
    if (!full) return NULL;
    {
        Pos saved = self->p;
        int rc = board_set_fen(self, full);
        Py_DECREF(full);
        if (rc < 0) return NULL;
        /* Only the placement is being replaced; keep the rest of the state. */
        saved.pawns = self->p.pawns;
        saved.knights = self->p.knights;
        saved.bishops = self->p.bishops;
        saved.rooks = self->p.rooks;
        saved.queens = self->p.queens;
        saved.kings = self->p.kings;
        saved.white = self->p.white;
        saved.black = self->p.black;
        self->p = saved;
        self->stack_len = 0;
    }
    result = Py_None;
    Py_INCREF(result);
    return result;
}

static PyObject *Board_public_set_piece_at(BoardObject *self, PyObject *args, PyObject *kwds)
{
    static char *kwlist[] = { "square", "piece", "promoted", NULL };
    PyObject *piece, *promoted = Py_False;
    int sq;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "iO|O", kwlist,
                                     &sq, &piece, &promoted))
        return NULL;
    if (check_square_range(sq) < 0) return NULL;

    if (piece == Py_None) {
        clear_square(&self->p, sq);
        Py_RETURN_NONE;
    }
    if (!PyObject_TypeCheck(piece, &PieceType)) {
        PyErr_Format(PyExc_TypeError, "expected Piece or None, not %.200s",
                     Py_TYPE(piece)->tp_name);
        return NULL;
    }
    set_piece(&self->p, sq, ((PieceObject *)piece)->piece_type,
              ((PieceObject *)piece)->color);
    Py_RETURN_NONE;
}

static PyObject *Board_set_piece_map(BoardObject *self, PyObject *arg)
{
    PyObject *key, *value;
    Py_ssize_t pos = 0;

    if (!PyDict_Check(arg)) {
        PyErr_Format(PyExc_TypeError, "expected a dict of square -> Piece, not %.200s",
                     Py_TYPE(arg)->tp_name);
        return NULL;
    }
    self->p.pawns = self->p.knights = self->p.bishops = 0;
    self->p.rooks = self->p.queens = self->p.kings = 0;
    self->p.white = self->p.black = 0;
    self->stack_len = 0;

    while (PyDict_Next(arg, &pos, &key, &value)) {
        long sq = PyLong_AsLong(key);
        if (sq == -1 && PyErr_Occurred()) return NULL;
        if (check_square_range((int)sq) < 0) return NULL;
        if (!PyObject_TypeCheck(value, &PieceType)) {
            PyErr_Format(PyExc_TypeError, "expected Piece, not %.200s",
                         Py_TYPE(value)->tp_name);
            return NULL;
        }
        set_piece(&self->p, (int)sq, ((PieceObject *)value)->piece_type,
                  ((PieceObject *)value)->color);
    }
    Py_RETURN_NONE;
}

/* transforms */

static void pos_apply_mirror(Pos *p)
{
    U64 w;
    p->pawns = flip_vertical_bb(p->pawns);
    p->knights = flip_vertical_bb(p->knights);
    p->bishops = flip_vertical_bb(p->bishops);
    p->rooks = flip_vertical_bb(p->rooks);
    p->queens = flip_vertical_bb(p->queens);
    p->kings = flip_vertical_bb(p->kings);
    w = flip_vertical_bb(p->white);
    p->white = flip_vertical_bb(p->black);
    p->black = w;

    {
        int c = p->castling, nc = 0;
        if (c & CR_WK) nc |= CR_BK;
        if (c & CR_WQ) nc |= CR_BQ;
        if (c & CR_BK) nc |= CR_WK;
        if (c & CR_BQ) nc |= CR_WQ;
        p->castling = nc;
    }
    if (p->ep >= 0) p->ep ^= 56;
    p->turn = !p->turn;
}

static PyObject *Board_apply_mirror(BoardObject *self, PyObject *Py_UNUSED(i))
{
    pos_apply_mirror(&self->p);
    self->stack_len = 0;
    Py_RETURN_NONE;
}

static PyObject *Board_mirror(BoardObject *self, PyObject *Py_UNUSED(i))
{
    BoardObject *b = board_alloc_copy(self, 0);
    if (!b) return NULL;
    pos_apply_mirror(&b->p);
    return (PyObject *)b;
}

/* `f` maps bitboard to bitboard; castling rides along as rook squares. */
static int pos_apply_transform(Pos *p, PyObject *f)
{
    U64 *fields[8];
    U64 rooks_bb;
    int i, c, k;

    fields[0] = &p->pawns;  fields[1] = &p->knights; fields[2] = &p->bishops;
    fields[3] = &p->rooks;  fields[4] = &p->queens;  fields[5] = &p->kings;
    fields[6] = &p->white;  fields[7] = &p->black;

    rooks_bb = 0;
    for (c = 0; c < 2; c++)
        for (k = 0; k < 2; k++)
            if (p->castling & CASTLE_RIGHT[c][k])
                rooks_bb |= BB(CASTLE_ROOK_FROM[c][k]);

    for (i = 0; i < 8; i++) {
        PyObject *arg = PyLong_FromUnsignedLongLong(*fields[i]);
        PyObject *res;
        if (!arg) return -1;
        res = PyObject_CallOneArg(f, arg);
        Py_DECREF(arg);
        if (!res) return -1;
        *fields[i] = PyLong_AsUnsignedLongLongMask(res);
        Py_DECREF(res);
        if (PyErr_Occurred()) return -1;
    }

    {
        PyObject *arg = PyLong_FromUnsignedLongLong(rooks_bb);
        PyObject *res;
        if (!arg) return -1;
        res = PyObject_CallOneArg(f, arg);
        Py_DECREF(arg);
        if (!res) return -1;
        rooks_bb = PyLong_AsUnsignedLongLongMask(res);
        Py_DECREF(res);
        if (PyErr_Occurred()) return -1;
    }

    p->castling = 0;
    if (rooks_bb & BB(SQ_H1)) p->castling |= CR_WK;
    if (rooks_bb & BB(SQ_A1)) p->castling |= CR_WQ;
    if (rooks_bb & BB(SQ_H8)) p->castling |= CR_BK;
    if (rooks_bb & BB(SQ_A8)) p->castling |= CR_BQ;

    if (p->ep >= 0) {
        PyObject *arg = PyLong_FromUnsignedLongLong(BB(p->ep));
        PyObject *res;
        U64 m;
        if (!arg) return -1;
        res = PyObject_CallOneArg(f, arg);
        Py_DECREF(arg);
        if (!res) return -1;
        m = PyLong_AsUnsignedLongLongMask(res);
        Py_DECREF(res);
        if (PyErr_Occurred()) return -1;
        p->ep = m ? msb64(m) : -1;
    }
    return 0;
}

static PyObject *Board_apply_transform(BoardObject *self, PyObject *f)
{
    if (pos_apply_transform(&self->p, f) < 0) return NULL;
    self->stack_len = 0;
    Py_RETURN_NONE;
}

static PyObject *Board_transform(BoardObject *self, PyObject *f)
{
    BoardObject *b = board_alloc_copy(self, 0);
    if (!b) return NULL;
    if (pos_apply_transform(&b->p, f) < 0) { Py_DECREF(b); return NULL; }
    return (PyObject *)b;
}

/* filtered generation */

static void filter_moves(MoveList *src, MoveList *dst, U64 from_mask, U64 to_mask)
{
    int i;
    for (i = 0; i < src->n; i++) {
        int m = src->v[i];
        if (!(BB(MV_FROM(m)) & from_mask)) continue;
        if (!(BB(MV_TO(m)) & to_mask)) continue;
        ml_add(dst, MV_FROM(m), MV_TO(m), MV_PROMO(m));
    }
}

static int parse_masks(PyObject *args, PyObject *kwds, U64 *from_mask, U64 *to_mask)
{
    static char *kwlist[] = { "from_mask", "to_mask", NULL };
    PyObject *fm = NULL, *tm = NULL;

    *from_mask = BB_ALL;
    *to_mask = BB_ALL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|OO", kwlist, &fm, &tm))
        return -1;
    if (fm && coerce_mask(fm, from_mask) != 0) {
        PyErr_SetString(PyExc_TypeError, "from_mask must be an int or SquareSet");
        return -1;
    }
    if (tm && coerce_mask(tm, to_mask) != 0) {
        PyErr_SetString(PyExc_TypeError, "to_mask must be an int or SquareSet");
        return -1;
    }
    return 0;
}

static PyObject *Board_generate_legal_moves(BoardObject *self, PyObject *args, PyObject *kwds)
{
    MoveList all, out;
    U64 from_mask, to_mask;

    if (parse_masks(args, kwds, &from_mask, &to_mask) < 0) return NULL;
    ml_init(&all);
    ml_init(&out);
    gen_legal(&self->p, &all);
    filter_moves(&all, &out, from_mask, to_mask);
    ml_free(&all);
    return moveiter_from_list(&out);
}

static PyObject *Board_generate_pseudo_legal_moves(BoardObject *self, PyObject *args, PyObject *kwds)
{
    MoveList all, out;
    U64 from_mask, to_mask;

    if (parse_masks(args, kwds, &from_mask, &to_mask) < 0) return NULL;
    ml_init(&all);
    ml_init(&out);
    gen_pseudo(&self->p, &all);
    filter_moves(&all, &out, from_mask, to_mask);
    ml_free(&all);
    return moveiter_from_list(&out);
}

/* Ordinary captures first, then en passant, as python-chess orders them. */
static PyObject *gen_captures(BoardObject *self, PyObject *args, PyObject *kwds,
                              int legal_only)
{
    MoveList all, out;
    U64 from_mask, to_mask, theirs;
    int i;

    if (parse_masks(args, kwds, &from_mask, &to_mask) < 0) return NULL;
    theirs = self->p.turn ? self->p.black : self->p.white;

    ml_init(&all);
    ml_init(&out);
    if (legal_only) gen_legal(&self->p, &all);
    else gen_pseudo(&self->p, &all);

    filter_moves(&all, &out, from_mask, to_mask & theirs);
    for (i = 0; i < all.n; i++) {
        int m = all.v[i];
        if (!(BB(MV_FROM(m)) & from_mask)) continue;
        if (!(BB(MV_TO(m)) & to_mask)) continue;
        if (board_is_en_passant(&self->p, MV_FROM(m), MV_TO(m)))
            ml_add(&out, MV_FROM(m), MV_TO(m), MV_PROMO(m));
    }
    ml_free(&all);
    return moveiter_from_list(&out);
}

static PyObject *Board_generate_legal_captures(BoardObject *self, PyObject *args, PyObject *kwds)
{
    return gen_captures(self, args, kwds, 1);
}

static PyObject *Board_generate_pseudo_legal_captures(BoardObject *self, PyObject *args, PyObject *kwds)
{
    return gen_captures(self, args, kwds, 0);
}

static PyObject *gen_ep(BoardObject *self, PyObject *args, PyObject *kwds, int legal_only)
{
    MoveList all, out;
    U64 from_mask, to_mask;
    int i;

    if (parse_masks(args, kwds, &from_mask, &to_mask) < 0) return NULL;
    ml_init(&all);
    ml_init(&out);
    if (legal_only) gen_legal(&self->p, &all);
    else gen_pseudo(&self->p, &all);
    for (i = 0; i < all.n; i++) {
        int m = all.v[i];
        if (!(BB(MV_FROM(m)) & from_mask)) continue;
        if (!(BB(MV_TO(m)) & to_mask)) continue;
        if (board_is_en_passant(&self->p, MV_FROM(m), MV_TO(m)))
            ml_add(&out, MV_FROM(m), MV_TO(m), MV_PROMO(m));
    }
    ml_free(&all);
    return moveiter_from_list(&out);
}

static PyObject *Board_generate_legal_ep(BoardObject *self, PyObject *args, PyObject *kwds)
{
    return gen_ep(self, args, kwds, 1);
}

static PyObject *Board_generate_pseudo_legal_ep(BoardObject *self, PyObject *args, PyObject *kwds)
{
    return gen_ep(self, args, kwds, 0);
}

/* EPD */

/* FEN without the counters; ep only when the capture is legal. */
static PyObject *epd_position(BoardObject *self)
{
    const Pos *p = &self->p;
    char buf[96];
    Py_ssize_t n = board_fen_str(p, buf);

    buf[n++] = ' ';
    buf[n++] = p->turn ? 'w' : 'b';
    buf[n++] = ' ';
    if (p->castling & CR_WK) buf[n++] = 'K';
    if (p->castling & CR_WQ) buf[n++] = 'Q';
    if (p->castling & CR_BK) buf[n++] = 'k';
    if (p->castling & CR_BQ) buf[n++] = 'q';
    if (!(p->castling & CR_ALL)) buf[n++] = '-';
    buf[n++] = ' ';
    if (p->ep >= 0 && board_has_legal_ep(p)) {
        buf[n++] = SQ_NAMES[p->ep][0];
        buf[n++] = SQ_NAMES[p->ep][1];
    } else {
        buf[n++] = '-';
    }
    return PyUnicode_FromStringAndSize(buf, n);
}

static PyObject *epd_quote(PyObject *s)
{
    PyObject *out = PyUnicode_FromString("\"");
    Py_ssize_t i, len;
    const char *src;

    if (!out) return NULL;
    src = PyUnicode_AsUTF8AndSize(s, &len);
    if (!src) { Py_DECREF(out); return NULL; }
    for (i = 0; i < len; i++) {
        const char *rep = NULL;
        char one[2];
        switch (src[i]) {
            case '\\': rep = "\\\\"; break;
            case '\t': rep = "\\t"; break;
            case '\r': rep = "\\r"; break;
            case '\n': rep = "\\n"; break;
            case '"':  rep = "\\\""; break;
            default: one[0] = src[i]; one[1] = '\0'; rep = one; break;
        }
        {
            PyObject *piece = PyUnicode_FromString(rep);
            if (!piece) { Py_DECREF(out); return NULL; }
            {
                PyObject *joined = PyUnicode_Concat(out, piece);
                Py_DECREF(piece);
                Py_DECREF(out);
                if (!joined) return NULL;
                out = joined;
            }
        }
    }
    {
        PyObject *tail = PyUnicode_FromString("\"");
        PyObject *joined;
        if (!tail) { Py_DECREF(out); return NULL; }
        joined = PyUnicode_Concat(out, tail);
        Py_DECREF(tail);
        Py_DECREF(out);
        return joined;
    }
}

static PyObject *epd_operand(BoardObject *self, PyObject *value)
{
    if (value == Py_None) return NULL;   /* caller emits a bare opcode */

    if (PyObject_TypeCheck(value, &MoveType)) {
        MoveObject *m = (MoveObject *)value;
        return board_san(self, m->from_square, m->to_square, m->promotion);
    }
    if (PyList_Check(value) || PyTuple_Check(value)) {
        PyObject *seq = PySequence_Fast(value, "expected a sequence");
        PyObject *parts, *sep, *joined;
        Py_ssize_t i, n;
        BoardObject *tmp;

        if (!seq) return NULL;
        n = PySequence_Fast_GET_SIZE(seq);
        parts = PyList_New(0);
        tmp = board_alloc_copy(self, 0);
        if (!parts || !tmp) { Py_XDECREF(parts); Py_XDECREF(tmp); Py_DECREF(seq); return NULL; }
        for (i = 0; i < n; i++) {
            PyObject *item = PySequence_Fast_GET_ITEM(seq, i);
            PyObject *s;
            if (PyObject_TypeCheck(item, &MoveType)) {
                MoveObject *m = (MoveObject *)item;
                s = board_san(tmp, m->from_square, m->to_square, m->promotion);
                if (s) do_move(&tmp->p, m->from_square, m->to_square, m->promotion);
            } else {
                s = PyObject_Str(item);
            }
            if (!s || PyList_Append(parts, s) < 0) {
                Py_XDECREF(s); Py_DECREF(parts); Py_DECREF(tmp); Py_DECREF(seq);
                return NULL;
            }
            Py_DECREF(s);
        }
        Py_DECREF(tmp);
        Py_DECREF(seq);
        sep = PyUnicode_FromString(" ");
        joined = sep ? PyUnicode_Join(sep, parts) : NULL;
        Py_XDECREF(sep);
        Py_DECREF(parts);
        return joined;
    }
    if (PyUnicode_Check(value)) return epd_quote(value);
    if (PyLong_Check(value) || PyFloat_Check(value)) return PyObject_Str(value);
    return PyObject_Str(value);
}

static PyObject *Board_epd(BoardObject *self, PyObject *args, PyObject *kwds)
{
    PyObject *out = epd_position(self);
    PyObject *key, *value;
    Py_ssize_t pos = 0;

    if (!out) return NULL;
    if (PyTuple_GET_SIZE(args) != 0) {
        Py_DECREF(out);
        PyErr_SetString(PyExc_TypeError,
                        "epd() takes no positional arguments; pass operations as keywords");
        return NULL;
    }
    if (!kwds) return out;

    while (PyDict_Next(kwds, &pos, &key, &value)) {
        PyObject *operand = epd_operand(self, value);
        PyObject *chunk;
        if (!operand && PyErr_Occurred()) { Py_DECREF(out); return NULL; }
        if (operand) {
            chunk = PyUnicode_FromFormat(" %U %U;", key, operand);
            Py_DECREF(operand);
        } else {
            chunk = PyUnicode_FromFormat(" %U;", key);
        }
        if (!chunk) { Py_DECREF(out); return NULL; }
        {
            PyObject *joined = PyUnicode_Concat(out, chunk);
            Py_DECREF(chunk);
            Py_DECREF(out);
            if (!joined) return NULL;
            out = joined;
        }
    }
    return out;
}

static int epd_is_move_opcode(const char *op, Py_ssize_t n)
{
    return (n == 2 && (memcmp(op, "bm", 2) == 0 || memcmp(op, "am", 2) == 0 ||
                       memcmp(op, "pv", 2) == 0 || memcmp(op, "sv", 2) == 0 ||
                       memcmp(op, "sm", 2) == 0));
}

static int epd_is_sequence_opcode(const char *op, Py_ssize_t n)
{
    return (n == 2 && (memcmp(op, "pv", 2) == 0 || memcmp(op, "sv", 2) == 0));
}

static PyObject *epd_decode_operand(BoardObject *self, const char *op, Py_ssize_t oplen,
                                    const char *raw, Py_ssize_t rawlen)
{
    Py_ssize_t a = 0, b = rawlen;

    while (a < b && is_ascii_space(raw[a])) a++;
    while (b > a && is_ascii_space(raw[b - 1])) b--;
    if (a == b) Py_RETURN_NONE;

    if (raw[a] == '"') {
        /* Quoted string: unescape and drop the delimiters. */
        PyObject *out = PyUnicode_FromString("");
        Py_ssize_t i = a + 1;
        if (!out) return NULL;
        while (i < b && raw[i] != '"') {
            char c = raw[i];
            const char *rep = NULL;
            char one[2];
            if (c == '\\' && i + 1 < b) {
                i++;
                switch (raw[i]) {
                    case 't': rep = "\t"; break;
                    case 'r': rep = "\r"; break;
                    case 'n': rep = "\n"; break;
                    default: one[0] = raw[i]; one[1] = '\0'; rep = one; break;
                }
            } else {
                one[0] = c; one[1] = '\0'; rep = one;
            }
            i++;
            {
                PyObject *piece = PyUnicode_FromString(rep);
                PyObject *joined;
                if (!piece) { Py_DECREF(out); return NULL; }
                joined = PyUnicode_Concat(out, piece);
                Py_DECREF(piece);
                Py_DECREF(out);
                if (!joined) return NULL;
                out = joined;
            }
        }
        return out;
    }

    if (epd_is_move_opcode(op, oplen)) {
        PyObject *list = PyList_New(0);
        BoardObject *tmp = board_alloc_copy(self, 0);
        int sequential = epd_is_sequence_opcode(op, oplen);
        Py_ssize_t i = a;

        if (!list || !tmp) { Py_XDECREF(list); Py_XDECREF(tmp); return NULL; }
        while (i < b) {
            Py_ssize_t s = i;
            PyObject *token, *move;
            while (i < b && !is_ascii_space(raw[i])) i++;
            token = PyUnicode_FromStringAndSize(raw + s, i - s);
            if (!token) { Py_DECREF(list); Py_DECREF(tmp); return NULL; }
            move = Board_parse_san(tmp, token);
            Py_DECREF(token);
            if (!move) {
                /* Unparseable operands are data, not errors. */
                PyErr_Clear();
                Py_DECREF(list);
                Py_DECREF(tmp);
                return PyUnicode_FromStringAndSize(raw + a, b - a);
            }
            if (PyList_Append(list, move) < 0) {
                Py_DECREF(move); Py_DECREF(list); Py_DECREF(tmp); return NULL;
            }
            if (sequential) {
                MoveObject *m = (MoveObject *)move;
                do_move(&tmp->p, m->from_square, m->to_square, m->promotion);
            }
            Py_DECREF(move);
            while (i < b && is_ascii_space(raw[i])) i++;
        }
        Py_DECREF(tmp);
        return list;
    }

    /* A single numeric token becomes a number; anything else stays text. */
    {
        Py_ssize_t i = a;
        int only_token = 1, is_int = 1, is_float = 0;
        while (i < b && !is_ascii_space(raw[i])) i++;
        if (i != b) only_token = 0;
        if (only_token) {
            Py_ssize_t k;
            for (k = a; k < b; k++) {
                char c = raw[k];
                if (c >= '0' && c <= '9') continue;
                if ((c == '-' || c == '+') && k == a) continue;
                if (c == '.' || c == 'e' || c == 'E') { is_int = 0; is_float = 1; continue; }
                is_int = 0; is_float = 0;
                break;
            }
            if (k == b && (is_int || is_float)) {
                char numbuf[64];
                if (b - a < (Py_ssize_t)sizeof(numbuf)) {
                    memcpy(numbuf, raw + a, (size_t)(b - a));
                    numbuf[b - a] = '\0';
                    if (is_int) {
                        char *endp;
                        long v = strtol(numbuf, &endp, 10);
                        if (*endp == '\0') return PyLong_FromLong(v);
                    } else {
                        char *endp;
                        double v = strtod(numbuf, &endp);
                        if (*endp == '\0') return PyFloat_FromDouble(v);
                    }
                }
            }
        }
    }

    return PyUnicode_FromStringAndSize(raw + a, b - a);
}

static PyObject *Board_clean_castling_rights(BoardObject *self, PyObject *Py_UNUSED(i))
{
    return PyLong_FromUnsignedLongLong(clean_castling_rooks(&self->p));
}

static PyObject *Board_set_epd(BoardObject *self, PyObject *arg);

static PyObject *Board_from_epd(PyTypeObject *cls, PyObject *arg)
{
    PyObject *board = Board_empty(cls, NULL);
    PyObject *ops, *result;

    if (!board) return NULL;
    ops = Board_set_epd((BoardObject *)board, arg);
    if (!ops) { Py_DECREF(board); return NULL; }
    result = PyTuple_Pack(2, board, ops);
    Py_DECREF(board);
    Py_DECREF(ops);
    return result;
}

static PyObject *Board_set_epd(BoardObject *self, PyObject *arg)
{
    const char *s;
    Py_ssize_t len, i = 0, field_start, nfields = 0;
    Py_ssize_t field_pos[4], field_len[4];
    PyObject *ops, *fen, *hmvc, *fmvn;
    long halfmove = 0, fullmove = 1;

    if (!PyUnicode_Check(arg)) {
        PyErr_Format(PyExc_TypeError, "EPD must be a string, not %.200s",
                     Py_TYPE(arg)->tp_name);
        return NULL;
    }
    s = PyUnicode_AsUTF8AndSize(arg, &len);
    if (!s) return NULL;

    while (i < len && nfields < 4) {
        while (i < len && is_ascii_space(s[i])) i++;
        if (i >= len) break;
        field_start = i;
        while (i < len && !is_ascii_space(s[i])) i++;
        field_pos[nfields] = field_start;
        field_len[nfields] = i - field_start;
        nfields++;
    }
    if (nfields < 4) {
        PyErr_Format(PyExc_ValueError,
                     "Invalid EPD: expected at least 4 fields, got %zd", nfields);
        return NULL;
    }

    {
        PyObject *position = PyUnicode_FromStringAndSize(
            s + field_pos[0], (field_pos[3] + field_len[3]) - field_pos[0]);
        if (!position) return NULL;
        fen = PyUnicode_FromFormat("%U 0 1", position);
        Py_DECREF(position);
        if (!fen) return NULL;
    }
    if (board_set_fen(self, fen) < 0) { Py_DECREF(fen); return NULL; }
    Py_DECREF(fen);

    /* Ops parse against the position, so set the board first. */
    ops = PyDict_New();
    if (!ops) return NULL;

    while (i < len) {
        Py_ssize_t op_start, op_len, val_start;
        PyObject *key, *value;
        int in_quotes = 0;

        while (i < len && (is_ascii_space(s[i]) || s[i] == ';')) i++;
        if (i >= len) break;
        op_start = i;
        while (i < len && !is_ascii_space(s[i]) && s[i] != ';') i++;
        op_len = i - op_start;
        if (op_len == 0) break;

        while (i < len && is_ascii_space(s[i])) i++;
        val_start = i;
        while (i < len) {
            if (s[i] == '"') in_quotes = !in_quotes;
            else if (s[i] == '\\' && in_quotes && i + 1 < len) i++;
            else if (s[i] == ';' && !in_quotes) break;
            i++;
        }

        key = PyUnicode_FromStringAndSize(s + op_start, op_len);
        if (!key) { Py_DECREF(ops); return NULL; }
        value = epd_decode_operand(self, s + op_start, op_len,
                                   s + val_start, i - val_start);
        if (!value) { Py_DECREF(key); Py_DECREF(ops); return NULL; }
        if (PyDict_SetItem(ops, key, value) < 0) {
            Py_DECREF(key); Py_DECREF(value); Py_DECREF(ops); return NULL;
        }
        Py_DECREF(key);
        Py_DECREF(value);
        if (i < len) i++;   /* step over the ';' */
    }

    hmvc = PyDict_GetItemString(ops, "hmvc");
    fmvn = PyDict_GetItemString(ops, "fmvn");
    if (hmvc && PyLong_Check(hmvc)) halfmove = PyLong_AsLong(hmvc);
    if (fmvn && PyLong_Check(fmvn)) fullmove = PyLong_AsLong(fmvn);
    if (halfmove >= 0) self->p.halfmove = (int)halfmove;
    if (fullmove >= 1) self->p.fullmove = (int)fullmove;

    return ops;
}

/* Board tables */

#define KWMETH(fn) ((PyCFunction)(void (*)(void))(fn))

static PyMethodDef Board_methods[] = {
    { "occupied_co", (PyCFunction)Board_occupied_co, METH_O,
      "Bitboard of squares occupied by a color." },
    { "pieces_mask", (PyCFunction)Board_pieces_mask, METH_VARARGS,
      "Get bitboard of specific piece type and color." },
    { "pieces", (PyCFunction)Board_pieces, METH_VARARGS,
      "Get set of squares with specific piece type and color." },
    { "piece_at", (PyCFunction)Board_piece_at, METH_O,
      "Get the piece at a square, or None if empty." },
    { "piece_type_at", (PyCFunction)Board_piece_type_at, METH_O,
      "Get the piece type at a square, or None if empty." },
    { "color_at", (PyCFunction)Board_color_at, METH_O,
      "Get the color of piece at a square, or None if empty." },
    { "piece_map", (PyCFunction)Board_piece_map, METH_NOARGS,
      "Get a dictionary mapping squares to pieces." },
    { "king", (PyCFunction)Board_king, METH_O,
      "Get the square of the king for a color." },
    { "attackers_mask", (PyCFunction)Board_attackers_mask, METH_VARARGS,
      "Get bitboard of pieces of given color attacking a square." },
    { "attackers", (PyCFunction)Board_attackers, METH_VARARGS,
      "Get set of squares with pieces of given color attacking a square." },
    { "is_attacked_by", (PyCFunction)Board_is_attacked_by, METH_VARARGS,
      "Check if a square is attacked by the given color." },
    { "attacks_mask", (PyCFunction)Board_attacks_mask, METH_O,
      "Get bitboard of squares attacked by the piece at sq." },
    { "is_check", (PyCFunction)Board_is_check, METH_NOARGS,
      "Check if the side to move is in check." },
    { "checkers", (PyCFunction)Board_checkers, METH_NOARGS,
      "Set of squares with pieces giving check." },
    { "is_into_check", (PyCFunction)Board_is_into_check, METH_O,
      "Check if making a move would leave the king in check." },
    { "gives_check", (PyCFunction)Board_gives_check, METH_O,
      "Check if a move gives check to the opponent." },
    { "is_checkmate", (PyCFunction)Board_is_checkmate, METH_NOARGS,
      "Check if the position is checkmate." },
    { "is_stalemate", (PyCFunction)Board_is_stalemate, METH_NOARGS,
      "Check if the position is stalemate." },
    { "is_insufficient_material", (PyCFunction)Board_is_insufficient_material,
      METH_NOARGS, "Check for insufficient material draw." },
    { "is_fifty_moves", (PyCFunction)Board_is_fifty_moves, METH_NOARGS,
      "Check for 50-move rule draw." },
    { "is_seventyfive_moves", (PyCFunction)Board_is_seventyfive_moves,
      METH_NOARGS, "Check for 75-move rule automatic draw." },
    { "is_game_over", KWMETH(Board_is_game_over), METH_VARARGS | METH_KEYWORDS,
      "Check if the game is over." },
    { "outcome", KWMETH(Board_outcome), METH_VARARGS | METH_KEYWORDS,
      "Get game outcome if game is over, otherwise None." },
    { "result", KWMETH(Board_result), METH_VARARGS | METH_KEYWORDS,
      "Get the game result: '1-0', '0-1', '1/2-1/2', or '*' while ongoing." },
    { "has_kingside_castling_rights", (PyCFunction)Board_has_kingside_castling_rights,
      METH_O, "Check if color has kingside castling rights." },
    { "has_queenside_castling_rights", (PyCFunction)Board_has_queenside_castling_rights,
      METH_O, "Check if color has queenside castling rights." },
    { "has_castling_rights", (PyCFunction)Board_has_castling_rights, METH_O,
      "Check if color has any castling rights." },
    { "is_legal", (PyCFunction)Board_is_legal, METH_O, "Check if a move is legal." },
    { "is_capture", (PyCFunction)Board_is_capture, METH_O,
      "Check if a move is a capture." },
    { "is_en_passant", (PyCFunction)Board_is_en_passant, METH_O,
      "Check if a move is an en passant capture." },
    { "is_castling", (PyCFunction)Board_is_castling, METH_O,
      "Check if a move is a castling move." },
    { "is_kingside_castling", (PyCFunction)Board_is_kingside_castling, METH_O,
      "Check if a move is kingside (short) castling." },
    { "is_queenside_castling", (PyCFunction)Board_is_queenside_castling, METH_O,
      "Check if a move is queenside (long) castling." },
    { "push", (PyCFunction)Board_push, METH_O, "Make a move on the board." },
    { "push_uci", (PyCFunction)Board_push_uci, METH_O, "Make a move from UCI string." },
    { "push_san", (PyCFunction)Board_push_san, METH_O, "Parse and make a SAN move." },
    { "pop", (PyCFunction)Board_pop, METH_NOARGS, "Undo the last move and return it." },
    { "peek", (PyCFunction)Board_peek, METH_NOARGS,
      "Peek at the last move without undoing it." },
    { "move_stack", (PyCFunction)Board_move_stack, METH_NOARGS,
      "Get a list of all moves made from the starting position." },
    { "clear_stack", (PyCFunction)Board_clear_stack, METH_NOARGS,
      "Clear the move stack without changing the position." },
    { "root", (PyCFunction)Board_root, METH_NOARGS,
      "Get a copy of the board at the root position (before any moves)." },
    { "is_repetition", (PyCFunction)Board_is_repetition, METH_VARARGS,
      "Check whether the current position has occurred `count` times." },
    { "is_fivefold_repetition", (PyCFunction)Board_is_fivefold_repetition,
      METH_NOARGS, "Check for automatic fivefold repetition draw." },
    { "can_claim_threefold_repetition",
      (PyCFunction)Board_can_claim_threefold_repetition, METH_NOARGS,
      "Check for threefold repetition draw claim." },
    { "can_claim_fifty_moves", (PyCFunction)Board_can_claim_fifty_moves,
      METH_NOARGS, "Check for a claimable fifty-move draw." },
    { "can_claim_draw", (PyCFunction)Board_can_claim_draw, METH_NOARGS,
      "Check if the current player can claim a draw." },
    { "set_fen", (PyCFunction)Board_set_fen, METH_O,
      "Set board position from FEN string." },
    { "fen", (PyCFunction)Board_fen, METH_NOARGS,
      "Generate FEN string for current position." },
    { "board_fen", (PyCFunction)Board_board_fen, METH_NOARGS,
      "Get just the board portion of the FEN." },
    { "copy", KWMETH(Board_copy), METH_VARARGS | METH_KEYWORDS,
      "Create a copy of the board." },
    { "__copy__", (PyCFunction)Board_copy_dunder, METH_NOARGS, NULL },
    { "unicode", KWMETH(Board_unicode), METH_VARARGS | METH_KEYWORDS,
      "Unicode board representation." },
    { "san", (PyCFunction)Board_san, METH_O,
      "Convert a move to Standard Algebraic Notation." },
    { "parse_san", (PyCFunction)Board_parse_san, METH_O,
      "Parse a SAN move string such as 'Nf3', 'exd5' or 'O-O'." },
    { "variation_san", (PyCFunction)Board_variation_san, METH_O,
      "Convert a variation (list of moves) to SAN string." },
    { "_generate_pseudo_legal_moves", (PyCFunction)Board_gen_pseudo, METH_NOARGS,
      "Generate all pseudo-legal moves (may leave king in check)." },
    { "_generate_legal_moves", (PyCFunction)Board_gen_legal, METH_NOARGS,
      "Generate all legal moves." },
    { "_get_piece_bb", (PyCFunction)Board_get_piece_bb, METH_O, NULL },
    { "_set_piece_bb", (PyCFunction)Board_set_piece_bb, METH_VARARGS, NULL },
    { "_clear_square", (PyCFunction)Board_clear_square, METH_O, NULL },
    { "_set_piece_at", (PyCFunction)Board_set_piece_at, METH_VARARGS, NULL },
    { "_remove_piece_at", (PyCFunction)Board_remove_piece_at, METH_O, NULL },
    { "_pinned_mask", (PyCFunction)Board_pinned_mask, METH_O, NULL },
    { "_can_castle_kingside", (PyCFunction)Board_can_castle_kingside, METH_O, NULL },
    { "_can_castle_queenside", (PyCFunction)Board_can_castle_queenside, METH_O, NULL },
    { "_position_hash", (PyCFunction)Board_position_hash, METH_NOARGS, NULL },

    /* --- inspection --- */
    { "attacks", (PyCFunction)Board_attacks, METH_O,
      "Set of squares attacked by the piece on the given square." },
    { "pin", (PyCFunction)Board_pin, METH_VARARGS,
      "Squares the piece on `square` is pinned along, or all squares." },
    { "pin_mask", (PyCFunction)Board_pin_mask, METH_VARARGS,
      "Bitboard of the pin ray, or all squares when the piece is not pinned." },
    { "is_pinned", (PyCFunction)Board_is_pinned, METH_VARARGS,
      "Whether the piece on the given square is absolutely pinned." },
    { "ply", (PyCFunction)Board_ply, METH_NOARGS,
      "Number of half-moves since the start of the game." },
    { "was_into_check", (PyCFunction)Board_was_into_check, METH_NOARGS,
      "Whether the side that just moved left its own king in check." },
    { "is_zeroing", (PyCFunction)Board_is_zeroing, METH_O,
      "Whether the move resets the halfmove clock (pawn move or capture)." },
    { "is_irreversible", (PyCFunction)Board_is_irreversible, METH_O,
      "Whether the move can never be taken back by a later position." },
    { "has_pseudo_legal_en_passant", (PyCFunction)Board_has_pseudo_legal_en_passant,
      METH_NOARGS, "Whether a pseudo-legal en passant capture exists." },
    { "has_legal_en_passant", (PyCFunction)Board_has_legal_en_passant,
      METH_NOARGS, "Whether a legal en passant capture exists." },
    { "has_insufficient_material", (PyCFunction)Board_has_insufficient_material,
      METH_O, "Whether the given colour alone cannot force checkmate." },
    { "is_pseudo_legal", (PyCFunction)Board_is_pseudo_legal, METH_O,
      "Whether the move is pseudo-legal (may still leave the king in check)." },
    { "status", (PyCFunction)Board_status, METH_NOARGS,
      "Bitmask of problems with the position; 0 when it is valid." },
    { "is_valid", (PyCFunction)Board_is_valid, METH_NOARGS,
      "Whether the position passes all basic validity checks." },
    { "zobrist_hash", (PyCFunction)Board_zobrist_hash, METH_NOARGS,
      "PolyGlot-compatible Zobrist hash of the position." },

    /* --- move parsing and notation --- */
    { "parse_uci", (PyCFunction)Board_parse_uci, METH_O,
      "Parse a UCI string and check that the move is legal here." },
    { "find_move", KWMETH(Board_find_move), METH_VARARGS | METH_KEYWORDS,
      "Find the legal move between two squares, defaulting promotion to queen." },
    { "lan", (PyCFunction)Board_lan, METH_O,
      "Long algebraic notation of a move, e.g. 'Ng1-f3'." },
    { "san_and_push", (PyCFunction)Board_san_and_push, METH_O,
      "Return the SAN of a move and play it." },
    { "epd", KWMETH(Board_epd), METH_VARARGS | METH_KEYWORDS,
      "EPD of the position, with any keyword arguments as operations." },
    { "set_epd", (PyCFunction)Board_set_epd, METH_O,
      "Set the position from an EPD and return its operations as a dict." },
    { "from_epd", (PyCFunction)Board_from_epd, METH_O | METH_CLASS,
      "Build a board from an EPD; returns (board, operations)." },
    { "clean_castling_rights", (PyCFunction)Board_clean_castling_rights, METH_NOARGS,
      "Bitboard of rook squares whose castling rights are actually backed by "
      "a king and rook." },

    /* --- editing --- */
    { "empty", (PyCFunction)Board_empty, METH_NOARGS | METH_CLASS,
      "Create a board with no pieces and no castling rights." },
    { "clear", (PyCFunction)Board_clear, METH_NOARGS,
      "Clear the board and reset all state." },
    { "clear_board", (PyCFunction)Board_clear_board, METH_NOARGS,
      "Remove every piece, leaving the rest of the state alone." },
    { "reset", (PyCFunction)Board_reset, METH_NOARGS,
      "Restore the starting position and state." },
    { "reset_board", (PyCFunction)Board_reset_board, METH_NOARGS,
      "Restore the starting piece placement only." },
    { "set_board_fen", (PyCFunction)Board_set_board_fen, METH_O,
      "Set the piece placement from the board part of a FEN." },
    { "set_piece_at", KWMETH(Board_public_set_piece_at), METH_VARARGS | METH_KEYWORDS,
      "Put a piece on a square, or remove one by passing None." },
    { "remove_piece_at", (PyCFunction)Board_remove_piece_at, METH_O,
      "Remove and return the piece on a square." },
    { "set_piece_map", (PyCFunction)Board_set_piece_map, METH_O,
      "Replace the position with a dict of square -> Piece." },

    /* --- transforms --- */
    { "mirror", (PyCFunction)Board_mirror, METH_NOARGS,
      "Copy of the board flipped vertically with colours swapped." },
    { "apply_mirror", (PyCFunction)Board_apply_mirror, METH_NOARGS,
      "Mirror this board in place." },
    { "transform", (PyCFunction)Board_transform, METH_O,
      "Copy of the board with a bitboard transform applied." },
    { "apply_transform", (PyCFunction)Board_apply_transform, METH_O,
      "Apply a bitboard transform to this board in place." },

    /* --- filtered generation --- */
    { "generate_legal_moves", KWMETH(Board_generate_legal_moves),
      METH_VARARGS | METH_KEYWORDS,
      "Legal moves restricted to the given from/to bitboards." },
    { "generate_pseudo_legal_moves", KWMETH(Board_generate_pseudo_legal_moves),
      METH_VARARGS | METH_KEYWORDS,
      "Pseudo-legal moves restricted to the given from/to bitboards." },
    { "generate_legal_captures", KWMETH(Board_generate_legal_captures),
      METH_VARARGS | METH_KEYWORDS, "Legal captures, including en passant." },
    { "generate_pseudo_legal_captures", KWMETH(Board_generate_pseudo_legal_captures),
      METH_VARARGS | METH_KEYWORDS, "Pseudo-legal captures, including en passant." },
    { "generate_legal_ep", KWMETH(Board_generate_legal_ep),
      METH_VARARGS | METH_KEYWORDS, "Legal en passant captures." },
    { "generate_pseudo_legal_ep", KWMETH(Board_generate_pseudo_legal_ep),
      METH_VARARGS | METH_KEYWORDS, "Pseudo-legal en passant captures." },
    { NULL }
};

static PyGetSetDef Board_getset[] = {
    { "turn", (getter)Board_get_turn, (setter)Board_set_turn,
      "Side to move (WHITE=True, BLACK=False).", NULL },
    { "castling_rights", (getter)Board_get_castling_rights,
      (setter)Board_set_castling_rights, "Castling right bits.", NULL },
    { "ep_square", (getter)Board_get_ep, (setter)Board_set_ep,
      "En passant target square, or None.", NULL },
    { "halfmove_clock", (getter)Board_get_halfmove_clock,
      (setter)Board_set_halfmove_clock, "Plies since the last pawn move or capture.", NULL },
    { "fullmove_number", (getter)Board_get_fullmove_number,
      (setter)Board_set_fullmove_number, "Full move number.", NULL },
    { "occupied", (getter)Board_get_occupied, NULL,
      "Bitboard of all occupied squares.", NULL },
    { "checkers_mask", (getter)Board_get_checkers_mask, NULL,
      "Bitboard of pieces giving check to the side to move.", NULL },
    { "legal_moves", (getter)Board_get_legal_moves, NULL,
      "The legal moves in this position.", NULL },
    { "pseudo_legal_moves", (getter)Board_get_pseudo_legal_moves, NULL,
      "The pseudo-legal moves in this position.", NULL },
    { "_pawns", (getter)Board_get__pawns, (setter)Board_set__pawns, NULL, NULL },
    { "_knights", (getter)Board_get__knights, (setter)Board_set__knights, NULL, NULL },
    { "_bishops", (getter)Board_get__bishops, (setter)Board_set__bishops, NULL, NULL },
    { "_rooks", (getter)Board_get__rooks, (setter)Board_set__rooks, NULL, NULL },
    { "_queens", (getter)Board_get__queens, (setter)Board_set__queens, NULL, NULL },
    { "_kings", (getter)Board_get__kings, (setter)Board_set__kings, NULL, NULL },
    { "_white", (getter)Board_get__white, (setter)Board_set__white, NULL, NULL },
    { "_black", (getter)Board_get__black, (setter)Board_set__black, NULL, NULL },
    { NULL }
};

static PyTypeObject BoardType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "bmm_chess._core.Board",
    sizeof(BoardObject),
};

/* module-level helpers */

static PyObject *m_square(PyObject *self, PyObject *args)
{
    int f, r;
    if (!PyArg_ParseTuple(args, "ii", &f, &r)) return NULL;
    return PyLong_FromLong(r * 8 + f);
}

static PyObject *m_square_file(PyObject *self, PyObject *arg)
{
    long sq = PyLong_AsLong(arg);
    if (sq == -1 && PyErr_Occurred()) return NULL;
    return PyLong_FromLong(sq & 7);
}

static PyObject *m_square_rank(PyObject *self, PyObject *arg)
{
    long sq = PyLong_AsLong(arg);
    if (sq == -1 && PyErr_Occurred()) return NULL;
    return PyLong_FromLong(sq >> 3);
}

static PyObject *m_square_name(PyObject *self, PyObject *arg)
{
    long sq = PyLong_AsLong(arg);
    if (sq == -1 && PyErr_Occurred()) return NULL;
    if (sq < 0) sq += 64;   /* SQUARE_NAMES is a list; keep list indexing */
    if (sq < 0 || sq > 63) {
        PyErr_SetString(PyExc_IndexError, "list index out of range");
        return NULL;
    }
    return PyUnicode_FromStringAndSize(SQ_NAMES[sq], 2);
}

static PyObject *m_parse_square(PyObject *self, PyObject *arg)
{
    const char *s;
    Py_ssize_t len;
    int sq;

    if (!PyUnicode_Check(arg)) {
        PyErr_Format(PyExc_TypeError, "square name must be a string, not %.200s",
                     Py_TYPE(arg)->tp_name);
        return NULL;
    }
    s = PyUnicode_AsUTF8AndSize(arg, &len);
    if (!s) return NULL;
    if (parse_square_str(s, len, &sq) < 0) return NULL;
    return PyLong_FromLong(sq);
}

static PyObject *m_square_distance(PyObject *self, PyObject *args)
{
    long a, b, df, dr;
    if (!PyArg_ParseTuple(args, "ll", &a, &b)) return NULL;
    df = (a & 7) - (b & 7);
    dr = (a >> 3) - (b >> 3);
    if (df < 0) df = -df;
    if (dr < 0) dr = -dr;
    return PyLong_FromLong(df > dr ? df : dr);
}

static PyObject *m_square_mirror(PyObject *self, PyObject *arg)
{
    long sq = PyLong_AsLong(arg);
    if (sq == -1 && PyErr_Occurred()) return NULL;
    return PyLong_FromLong(sq ^ 56);
}

static PyObject *m_piece_symbol(PyObject *self, PyObject *arg)
{
    long pt = PyLong_AsLong(arg);
    char c;
    if (pt == -1 && PyErr_Occurred()) return NULL;
    if (pt < 1 || pt > 6) return PyUnicode_FromStringAndSize("", 0);
    c = PIECE_CHARS[pt];
    return PyUnicode_FromStringAndSize(&c, 1);
}

static PyObject *m_piece_name(PyObject *self, PyObject *arg)
{
    long pt = PyLong_AsLong(arg);
    if (pt == -1 && PyErr_Occurred()) return NULL;
    if (pt < 1 || pt > 6) return PyUnicode_FromStringAndSize("", 0);
    return PyUnicode_FromString(PIECE_NAMES_C[pt]);
}

/* Braces open a comment token; parens are tokens; whitespace separates. */
static PyObject *m_tokenize_movetext(PyObject *self, PyObject *arg)
{
    PyObject *list;
    const void *data;
    int kind;
    Py_ssize_t len, i, start = -1;
    int in_comment = 0;

    if (!PyUnicode_Check(arg)) {
        PyErr_Format(PyExc_TypeError, "movetext must be a string, not %.200s",
                     Py_TYPE(arg)->tp_name);
        return NULL;
    }
#if PY_VERSION_HEX < 0x030C0000
    if (PyUnicode_READY(arg) < 0) return NULL;
#endif
    kind = PyUnicode_KIND(arg);
    data = PyUnicode_DATA(arg);
    len = PyUnicode_GET_LENGTH(arg);

    list = PyList_New(0);
    if (!list) return NULL;

#define FLUSH(end)                                                      \
    do {                                                                \
        if (start >= 0) {                                               \
            PyObject *tok = PyUnicode_Substring(arg, start, (end));      \
            if (!tok || PyList_Append(list, tok) < 0) {                 \
                Py_XDECREF(tok); Py_DECREF(list); return NULL;          \
            }                                                           \
            Py_DECREF(tok);                                             \
            start = -1;                                                 \
        }                                                               \
    } while (0)

    for (i = 0; i < len; i++) {
        Py_UCS4 ch = PyUnicode_READ(kind, data, i);
        if (in_comment) {
            if (ch == '}') {
                FLUSH(i + 1);
                in_comment = 0;
            }
        } else if (ch == '{') {
            FLUSH(i);
            start = i;
            in_comment = 1;
        } else if (ch == '(' || ch == ')') {
            FLUSH(i);
            start = i;
            FLUSH(i + 1);
        } else if (Py_UNICODE_ISSPACE(ch)) {
            FLUSH(i);
        } else if (start < 0) {
            start = i;
        }
    }
    FLUSH(len);
#undef FLUSH

    return list;
}

static PyObject *m_perft(PyObject *self, PyObject *args)
{
    BoardObject *board;
    Pos p;
    int depth;
    uint64_t nodes;

    if (!PyArg_ParseTuple(args, "O!i", &BoardType, &board, &depth)) return NULL;
    if (depth < 0) {
        PyErr_SetString(PyExc_ValueError, "depth must be >= 0");
        return NULL;
    }
    p = board->p;
    Py_BEGIN_ALLOW_THREADS
    nodes = perft_rec(&p, depth);
    Py_END_ALLOW_THREADS
    return PyLong_FromUnsignedLongLong(nodes);
}

static int arg_bb(PyObject *o, U64 *out)
{
    *out = PyLong_AsUnsignedLongLongMask(o);
    return PyErr_Occurred() ? -1 : 0;
}

static PyObject *m_lsb(PyObject *self, PyObject *arg)
{
    U64 bb;
    if (arg_bb(arg, &bb) < 0) return NULL;
    if (!bb) { PyErr_SetString(PyExc_ValueError, "lsb of an empty bitboard"); return NULL; }
    return PyLong_FromLong(ctz64(bb));
}

static PyObject *m_msb(PyObject *self, PyObject *arg)
{
    U64 bb;
    if (arg_bb(arg, &bb) < 0) return NULL;
    if (!bb) { PyErr_SetString(PyExc_ValueError, "msb of an empty bitboard"); return NULL; }
    return PyLong_FromLong(msb64(bb));
}

static PyObject *m_popcount(PyObject *self, PyObject *arg)
{
    U64 bb;
    if (arg_bb(arg, &bb) < 0) return NULL;
    return PyLong_FromLong(popcount64(bb));
}

static PyObject *m_scan_forward(PyObject *self, PyObject *arg)
{
    SSIterObject *it;
    U64 bb;
    if (arg_bb(arg, &bb) < 0) return NULL;
    it = PyObject_New(SSIterObject, &SquareSetIterType);
    if (!it) return NULL;
    it->mask = bb;
    return (PyObject *)it;
}

static PyObject *m_scan_reversed(PyObject *self, PyObject *arg)
{
    U64 bb;
    PyObject *list, *it;
    Py_ssize_t i = 0;

    if (arg_bb(arg, &bb) < 0) return NULL;
    list = PyList_New(popcount64(bb));
    if (!list) return NULL;
    while (bb) {
        int sq = msb64(bb);
        PyObject *v = PyLong_FromLong(sq);
        bb ^= BB(sq);
        if (!v) { Py_DECREF(list); return NULL; }
        PyList_SET_ITEM(list, i++, v);
    }
    it = PySeqIter_New(list);
    Py_DECREF(list);
    return it;
}

static PyObject *m_between(PyObject *self, PyObject *args)
{
    int a, b;
    if (!PyArg_ParseTuple(args, "ii", &a, &b)) return NULL;
    if (check_square_range(a) < 0 || check_square_range(b) < 0) return NULL;
    return squareset_new_raw(BETWEEN[a][b]);
}

static PyObject *m_ray(PyObject *self, PyObject *args)
{
    int a, b;
    if (!PyArg_ParseTuple(args, "ii", &a, &b)) return NULL;
    if (check_square_range(a) < 0 || check_square_range(b) < 0) return NULL;
    return squareset_new_raw(RAY[a][b]);
}

static PyObject *m_square_manhattan_distance(PyObject *self, PyObject *args)
{
    long a, b, df, dr;
    if (!PyArg_ParseTuple(args, "ll", &a, &b)) return NULL;
    df = (a & 7) - (b & 7);
    dr = (a >> 3) - (b >> 3);
    if (df < 0) df = -df;
    if (dr < 0) dr = -dr;
    return PyLong_FromLong(df + dr);
}

static PyObject *m_square_knight_distance(PyObject *self, PyObject *args)
{
    long a, b, dx, dy, m;
    U64 corners = BB(SQ_A1) | BB(SQ_H1) | BB(SQ_A8) | BB(SQ_H8);

    if (!PyArg_ParseTuple(args, "ll", &a, &b)) return NULL;
    if (check_square_range((int)a) < 0 || check_square_range((int)b) < 0) return NULL;
    dx = (a & 7) - (b & 7);
    dy = (a >> 3) - (b >> 3);
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;

    if (dx + dy == 1) return PyLong_FromLong(3);
    if (dx == 2 && dy == 2) return PyLong_FromLong(4);
    if (dx == 1 && dy == 1 && ((BB(a) & corners) || (BB(b) & corners)))
        return PyLong_FromLong(4);

    {
        long c1 = (dx + 1) / 2, c2 = (dy + 1) / 2, c3 = (dx + dy + 2) / 3;
        m = c1 > c2 ? c1 : c2;
        if (c3 > m) m = c3;
    }
    return PyLong_FromLong(m + ((m + dx + dy) % 2));
}

#define SHIFT_FN(name, expr)                                    \
static PyObject *name(PyObject *self, PyObject *arg)            \
{                                                               \
    U64 b;                                                      \
    if (arg_bb(arg, &b) < 0) return NULL;                       \
    return PyLong_FromUnsignedLongLong(expr);                   \
}

SHIFT_FN(m_shift_down, b >> 8)
SHIFT_FN(m_shift_2_down, b >> 16)
SHIFT_FN(m_shift_up, b << 8)
SHIFT_FN(m_shift_2_up, b << 16)
SHIFT_FN(m_shift_right, (b << 1) & ~FILE_A_BB)
SHIFT_FN(m_shift_2_right, (b << 2) & ~FILE_A_BB & ~(FILE_A_BB << 1))
SHIFT_FN(m_shift_left, (b >> 1) & ~FILE_H_BB)
SHIFT_FN(m_shift_2_left, (b >> 2) & ~FILE_H_BB & ~(FILE_H_BB >> 1))
SHIFT_FN(m_shift_up_left, (b << 7) & ~FILE_H_BB)
SHIFT_FN(m_shift_up_right, (b << 9) & ~FILE_A_BB)
SHIFT_FN(m_shift_down_left, (b >> 9) & ~FILE_H_BB)
SHIFT_FN(m_shift_down_right, (b >> 7) & ~FILE_A_BB)
SHIFT_FN(m_flip_vertical, flip_vertical_bb(b))
SHIFT_FN(m_flip_horizontal, flip_horizontal_bb(b))
SHIFT_FN(m_flip_diagonal, flip_diagonal_bb(b))
SHIFT_FN(m_flip_anti_diagonal, flip_anti_diagonal_bb(b))

static PyObject *m_zobrist_hash(PyObject *self, PyObject *arg)
{
    if (!PyObject_TypeCheck(arg, &BoardType)) {
        PyErr_Format(PyExc_TypeError, "expected a Board, not %.200s",
                     Py_TYPE(arg)->tp_name);
        return NULL;
    }
    return PyLong_FromUnsignedLongLong(polyglot_hash(&((BoardObject *)arg)->p));
}

static PyMethodDef module_methods[] = {
    { "lsb", m_lsb, METH_O, "Index of the lowest set bit." },
    { "msb", m_msb, METH_O, "Index of the highest set bit." },
    { "popcount", m_popcount, METH_O, "Number of set bits." },
    { "scan_forward", m_scan_forward, METH_O, "Iterate set bits, lowest first." },
    { "scan_reversed", m_scan_reversed, METH_O, "Iterate set bits, highest first." },
    { "between", m_between, METH_VARARGS,
      "Squares strictly between two aligned squares." },
    { "ray", m_ray, METH_VARARGS, "The whole line through two aligned squares." },
    { "square_manhattan_distance", m_square_manhattan_distance, METH_VARARGS,
      "Manhattan (taxicab) distance between two squares." },
    { "square_knight_distance", m_square_knight_distance, METH_VARARGS,
      "Number of knight moves between two squares." },
    { "shift_up", m_shift_up, METH_O, NULL },
    { "shift_2_up", m_shift_2_up, METH_O, NULL },
    { "shift_down", m_shift_down, METH_O, NULL },
    { "shift_2_down", m_shift_2_down, METH_O, NULL },
    { "shift_left", m_shift_left, METH_O, NULL },
    { "shift_2_left", m_shift_2_left, METH_O, NULL },
    { "shift_right", m_shift_right, METH_O, NULL },
    { "shift_2_right", m_shift_2_right, METH_O, NULL },
    { "shift_up_left", m_shift_up_left, METH_O, NULL },
    { "shift_up_right", m_shift_up_right, METH_O, NULL },
    { "shift_down_left", m_shift_down_left, METH_O, NULL },
    { "shift_down_right", m_shift_down_right, METH_O, NULL },
    { "flip_vertical", m_flip_vertical, METH_O, NULL },
    { "flip_horizontal", m_flip_horizontal, METH_O, NULL },
    { "flip_diagonal", m_flip_diagonal, METH_O, NULL },
    { "flip_anti_diagonal", m_flip_anti_diagonal, METH_O, NULL },
    { "zobrist_hash", m_zobrist_hash, METH_O,
      "PolyGlot-compatible Zobrist hash of a board." },
    { "square", m_square, METH_VARARGS,
      "Create a square index from file and rank indices (0-7)." },
    { "square_file", m_square_file, METH_O, "Get the file index (0-7) of a square." },
    { "square_rank", m_square_rank, METH_O, "Get the rank index (0-7) of a square." },
    { "square_name", m_square_name, METH_O,
      "Get the algebraic name of a square (e.g., 'e4')." },
    { "parse_square", m_parse_square, METH_O,
      "Parse a square name (e.g., 'e4') to a square index." },
    { "square_distance", m_square_distance, METH_VARARGS,
      "Get the Chebyshev distance between two squares." },
    { "square_mirror", m_square_mirror, METH_O, "Mirror square vertically (flip rank)." },
    { "piece_symbol", m_piece_symbol, METH_O,
      "Get the symbol for a piece type (lowercase)." },
    { "piece_name", m_piece_name, METH_O, "Get the name for a piece type." },
    { "tokenize_movetext", m_tokenize_movetext, METH_O,
      "Tokenize PGN movetext into individual tokens." },
    { "perft", m_perft, METH_VARARGS,
      "Count leaf nodes of the legal move tree to `depth` from a board." },
    { NULL }
};

/* module init */

static void module_free(void *m)
{
    move_freelist_clear();
    piece_freelist_clear();
}

static struct PyModuleDef coremodule = {
    PyModuleDef_HEAD_INIT,
    "bmm_chess._core",
    "Native core for bmm_chess: bitboards, legal move generation, FEN and SAN.",
    -1,
    module_methods,
    NULL, NULL, NULL,
    module_free
};

static void setup_types(void)
{
    Move_as_number.nb_bool = (inquiry)Move_bool;

    MoveType.tp_basicsize = sizeof(MoveObject);
    MoveType.tp_dealloc = (destructor)Move_dealloc;
    MoveType.tp_repr = (reprfunc)Move_repr;
    MoveType.tp_as_number = &Move_as_number;
    MoveType.tp_hash = (hashfunc)Move_hash;
    MoveType.tp_str = (reprfunc)Move_str;
    MoveType.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    MoveType.tp_doc = "A chess move: from_square, to_square and optional promotion.";
    MoveType.tp_richcompare = Move_richcompare;
    MoveType.tp_methods = Move_methods;
    MoveType.tp_getset = Move_getset;
    MoveType.tp_init = (initproc)Move_init;
    MoveType.tp_new = Move_new;
    MoveType.tp_free = PyObject_Free;

    PieceType.tp_basicsize = sizeof(PieceObject);
    PieceType.tp_dealloc = (destructor)Piece_dealloc;
    PieceType.tp_repr = (reprfunc)Piece_repr;
    PieceType.tp_hash = (hashfunc)Piece_hash;
    PieceType.tp_str = (reprfunc)Piece_str;
    PieceType.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    PieceType.tp_doc = "A chess piece: piece_type and color.";
    PieceType.tp_richcompare = Piece_richcompare;
    PieceType.tp_methods = Piece_methods;
    PieceType.tp_getset = Piece_getset;
    PieceType.tp_init = (initproc)Piece_init;
    PieceType.tp_new = Piece_new;
    PieceType.tp_free = PyObject_Free;

    SS_as_number.nb_bool = (inquiry)SS_bool;
    SS_as_number.nb_int = (unaryfunc)SS_int;
    SS_as_number.nb_index = (unaryfunc)SS_int;
    SS_as_number.nb_or = SS_or;
    SS_as_number.nb_and = SS_and;
    SS_as_number.nb_xor = SS_xor;
    SS_as_number.nb_subtract = SS_sub;
    SS_as_sequence.sq_length = (lenfunc)SS_len;
    SS_as_sequence.sq_contains = (objobjproc)SS_contains;

    SquareSetType.tp_basicsize = sizeof(SquareSetObject);
    SquareSetType.tp_dealloc = (destructor)SS_dealloc;
    SquareSetType.tp_repr = (reprfunc)SS_repr;
    SquareSetType.tp_as_number = &SS_as_number;
    SquareSetType.tp_as_sequence = &SS_as_sequence;
    SquareSetType.tp_hash = (hashfunc)SS_hash;
    SquareSetType.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    SquareSetType.tp_doc = "Set of squares represented as a bitboard.";
    SquareSetType.tp_richcompare = SS_richcompare;
    SquareSetType.tp_iter = (getiterfunc)SS_iter;
    SquareSetType.tp_methods = SS_methods;
    SquareSetType.tp_getset = SS_getset;
    SquareSetType.tp_init = (initproc)SS_init;
    SquareSetType.tp_new = SS_new;
    SquareSetType.tp_free = PyObject_Free;

    SquareSetIterType.tp_basicsize = sizeof(SSIterObject);
    SquareSetIterType.tp_dealloc = (destructor)SS_dealloc;
    SquareSetIterType.tp_flags = Py_TPFLAGS_DEFAULT;
    SquareSetIterType.tp_iter = PyObject_SelfIter;
    SquareSetIterType.tp_iternext = (iternextfunc)SSIter_next;
    SquareSetIterType.tp_free = PyObject_Free;

    RipplerType.tp_name = "bmm_chess._core.CarryRippler";
    RipplerType.tp_basicsize = sizeof(RipplerObject);
    RipplerType.tp_dealloc = (destructor)SS_dealloc;
    RipplerType.tp_flags = Py_TPFLAGS_DEFAULT;
    RipplerType.tp_iter = PyObject_SelfIter;
    RipplerType.tp_iternext = (iternextfunc)Rippler_next;
    RipplerType.tp_free = PyObject_Free;

    MoveIterType.tp_basicsize = sizeof(MoveIterObject);
    MoveIterType.tp_dealloc = (destructor)MoveIter_dealloc;
    MoveIterType.tp_flags = Py_TPFLAGS_DEFAULT;
    MoveIterType.tp_iter = PyObject_SelfIter;
    MoveIterType.tp_iternext = (iternextfunc)MoveIter_next;
    MoveIterType.tp_methods = MoveIter_methods;
    MoveIterType.tp_free = PyObject_Free;

    BoardType.tp_basicsize = sizeof(BoardObject);
    BoardType.tp_dealloc = (destructor)Board_dealloc;
    BoardType.tp_repr = (reprfunc)Board_repr;
    BoardType.tp_str = (reprfunc)Board_str;
    BoardType.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    BoardType.tp_doc = "Chess board with bitboard representation.";
    BoardType.tp_methods = Board_methods;
    BoardType.tp_getset = Board_getset;
    BoardType.tp_init = (initproc)Board_init;
    BoardType.tp_new = Board_new;
    BoardType.tp_free = PyObject_Free;

    LMG_as_sequence.sq_length = (lenfunc)LMG_len;
    LMG_as_sequence.sq_contains = (objobjproc)LMG_contains;
    LMG_as_number.nb_bool = (inquiry)LMG_bool;

    LMGType.tp_basicsize = sizeof(LMGObject);
    LMGType.tp_dealloc = (destructor)LMG_dealloc;
    LMGType.tp_repr = (reprfunc)LMG_repr;
    LMGType.tp_as_number = &LMG_as_number;
    LMGType.tp_as_sequence = &LMG_as_sequence;
    LMGType.tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE;
    LMGType.tp_doc = "The legal moves of a position, generated on demand.";
    LMGType.tp_iter = (getiterfunc)LMG_iter;
    LMGType.tp_methods = LMG_methods;
    LMGType.tp_getset = LMG_getset;
    LMGType.tp_new = LMG_new;
    LMGType.tp_free = PyObject_Free;
}

PyMODINIT_FUNC PyInit__core(void)
{
    PyObject *m;

    init_tables();
    setup_types();

    if (PyType_Ready(&MoveType) < 0) return NULL;
    if (PyType_Ready(&PieceType) < 0) return NULL;
    if (PyType_Ready(&SquareSetType) < 0) return NULL;
    if (PyType_Ready(&SquareSetIterType) < 0) return NULL;
    if (PyType_Ready(&RipplerType) < 0) return NULL;
    if (PyType_Ready(&MoveIterType) < 0) return NULL;
    if (PyType_Ready(&BoardType) < 0) return NULL;
    if (PyType_Ready(&LMGType) < 0) return NULL;

    m = PyModule_Create(&coremodule);
    if (!m) return NULL;

    Py_INCREF(&MoveType);
    Py_INCREF(&PieceType);
    Py_INCREF(&SquareSetType);
    Py_INCREF(&BoardType);
    Py_INCREF(&LMGType);
    if (PyModule_AddObject(m, "Move", (PyObject *)&MoveType) < 0 ||
        PyModule_AddObject(m, "Piece", (PyObject *)&PieceType) < 0 ||
        PyModule_AddObject(m, "SquareSet", (PyObject *)&SquareSetType) < 0 ||
        PyModule_AddObject(m, "Board", (PyObject *)&BoardType) < 0 ||
        PyModule_AddObject(m, "LegalMoveGenerator", (PyObject *)&LMGType) < 0) {
        Py_DECREF(m);
        return NULL;
    }

    PyModule_AddStringConstant(m, "STARTING_FEN", STARTING_FEN_C);

    /* All ValueError subclasses, so existing `except ValueError` keeps working. */
    InvalidMoveError = PyErr_NewException("bmm_chess.InvalidMoveError", PyExc_ValueError, NULL);
    IllegalMoveError = PyErr_NewException("bmm_chess.IllegalMoveError", PyExc_ValueError, NULL);
    AmbiguousMoveError = PyErr_NewException("bmm_chess.AmbiguousMoveError", PyExc_ValueError, NULL);
    if (!InvalidMoveError || !IllegalMoveError || !AmbiguousMoveError) {
        Py_DECREF(m);
        return NULL;
    }
    Py_INCREF(InvalidMoveError);
    Py_INCREF(IllegalMoveError);
    Py_INCREF(AmbiguousMoveError);
    if (PyModule_AddObject(m, "InvalidMoveError", InvalidMoveError) < 0 ||
        PyModule_AddObject(m, "IllegalMoveError", IllegalMoveError) < 0 ||
        PyModule_AddObject(m, "AmbiguousMoveError", AmbiguousMoveError) < 0) {
        Py_DECREF(m);
        return NULL;
    }

    PyModule_AddIntConstant(m, "STATUS_VALID", 0);
    PyModule_AddIntConstant(m, "STATUS_NO_WHITE_KING", ST_NO_WHITE_KING);
    PyModule_AddIntConstant(m, "STATUS_NO_BLACK_KING", ST_NO_BLACK_KING);
    PyModule_AddIntConstant(m, "STATUS_TOO_MANY_KINGS", ST_TOO_MANY_KINGS);
    PyModule_AddIntConstant(m, "STATUS_TOO_MANY_WHITE_PAWNS", ST_TOO_MANY_WHITE_PAWNS);
    PyModule_AddIntConstant(m, "STATUS_TOO_MANY_BLACK_PAWNS", ST_TOO_MANY_BLACK_PAWNS);
    PyModule_AddIntConstant(m, "STATUS_PAWNS_ON_BACKRANK", ST_PAWNS_ON_BACKRANK);
    PyModule_AddIntConstant(m, "STATUS_TOO_MANY_WHITE_PIECES", ST_TOO_MANY_WHITE_PIECES);
    PyModule_AddIntConstant(m, "STATUS_TOO_MANY_BLACK_PIECES", ST_TOO_MANY_BLACK_PIECES);
    PyModule_AddIntConstant(m, "STATUS_BAD_CASTLING_RIGHTS", ST_BAD_CASTLING_RIGHTS);
    PyModule_AddIntConstant(m, "STATUS_INVALID_EP_SQUARE", ST_INVALID_EP_SQUARE);
    PyModule_AddIntConstant(m, "STATUS_OPPOSITE_CHECK", ST_OPPOSITE_CHECK);
    PyModule_AddIntConstant(m, "STATUS_EMPTY", ST_EMPTY);
    PyModule_AddIntConstant(m, "STATUS_TOO_MANY_CHECKERS", ST_TOO_MANY_CHECKERS);
    PyModule_AddIntConstant(m, "STATUS_IMPOSSIBLE_CHECK", ST_IMPOSSIBLE_CHECK);

    {
        PyObject *squares = PyList_New(64), *files = PyList_New(8), *ranks = PyList_New(8);
        int i;
        if (!squares || !files || !ranks) {
            Py_XDECREF(squares); Py_XDECREF(files); Py_XDECREF(ranks);
            Py_DECREF(m);
            return NULL;
        }
        for (i = 0; i < 64; i++)
            PyList_SET_ITEM(squares, i, PyLong_FromUnsignedLongLong(BB(i)));
        for (i = 0; i < 8; i++) {
            PyList_SET_ITEM(files, i, PyLong_FromUnsignedLongLong(FILE_A_BB << i));
            PyList_SET_ITEM(ranks, i, PyLong_FromUnsignedLongLong(RANK_1_BB << (8 * i)));
        }
        PyModule_AddObject(m, "BB_SQUARES", squares);
        PyModule_AddObject(m, "BB_FILES", files);
        PyModule_AddObject(m, "BB_RANKS", ranks);
    }

    PyModule_AddObject(m, "BB_EMPTY", PyLong_FromUnsignedLongLong(0));
    PyModule_AddObject(m, "BB_ALL", PyLong_FromUnsignedLongLong(BB_ALL));
    PyModule_AddObject(m, "BB_LIGHT_SQUARES", PyLong_FromUnsignedLongLong(BB_LIGHT_SQUARES));
    PyModule_AddObject(m, "BB_DARK_SQUARES", PyLong_FromUnsignedLongLong(BB_DARK_SQUARES));
    PyModule_AddObject(m, "BB_BACKRANKS", PyLong_FromUnsignedLongLong(BB_BACKRANKS));
    PyModule_AddObject(m, "BB_CORNERS", PyLong_FromUnsignedLongLong(
        BB(SQ_A1) | BB(SQ_H1) | BB(SQ_A8) | BB(SQ_H8)));
    PyModule_AddObject(m, "BB_CENTER", PyLong_FromUnsignedLongLong(
        BB(27) | BB(28) | BB(35) | BB(36)));

    return m;
}
