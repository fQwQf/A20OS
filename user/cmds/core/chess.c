/*
 * A20OS Native ABI — Command-line Chess with AI Engine
 *
 * A complete chess game with minimax alpha-beta pruning AI.
 * Built against liba20c (A20OS native C library).
 *
 * Usage: native-chess-<arch>
 *   Moves in coordinate notation: e2e4, e7-e8q (promotion)
 *   Commands: quit, new, undo, moves, help, flip
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#ifdef __GLIBC__
#include <termios.h>
#include <unistd.h>
#define HAS_TERMIOS 1
#endif

/* ======================================================================
 * Constants
 * ====================================================================== */

#define EMPTY  0
#define PAWN   1
#define KNIGHT 2
#define BISHOP 3
#define ROOK   4
#define QUEEN  5
#define KING   6

#define WHITE  8
#define BLACK  16
#define COLOR_MASK (WHITE | BLACK)
#define PIECE_MASK 0x07

#define PIECE(type, color) ((type) | (color))
#define PIECE_TYPE(p) ((p) & PIECE_MASK)
#define PIECE_COLOR(p) ((p) & COLOR_MASK)
#define IS_WHITE(p) (((p) & WHITE) != 0)
#define IS_BLACK(p) (((p) & BLACK) != 0)
#define IS_EMPTY(p) ((p) == EMPTY)

#define SQ(r, c) ((r) * 8 + (c))
#define ROW(sq) ((sq) / 8)
#define COL(sq) ((sq) % 8)

#define MAX_MOVES 256
#define MAX_UNDO   64
#define HASH_SIZE  (1 << 16)
#define INF  1000000

/* Material values */
#define VAL_PAWN   100
#define VAL_KNIGHT 320
#define VAL_BISHOP 330
#define VAL_ROOK   500
#define VAL_QUEEN  900
#define VAL_KING   20000

/* Search depth */
#define DEFAULT_DEPTH 5

/* Difficulty levels */
#define DIFF_EASY   0
#define DIFF_MEDIUM 1
#define DIFF_HARD   2
#define DIFF_EXPERT 3
#define NUM_DIFFICULTIES 4

/* ======================================================================
 * Types
 * ====================================================================== */

typedef struct {
    int from;
    int to;
    int piece;
    int captured;
    int promoted;    /* piece type promoted to, or EMPTY */
    int castle;      /* 0=none, 1= kingside, 2=queenside */
    int en_passant;  /* 1 if en passant capture */
    int prev_ep;     /* previous en passant square, -1 if none */
    int prev_castle; /* previous castling rights */
    int halfmove;    /* prev halfmove clock */
} Move;

typedef struct {
    int board[64];
    int side;          /* WHITE or BLACK */
    int ep_square;     /* en passant target square, -1 if none */
    int castle;        /* castling rights bitmask: 1=wk, 2=wq, 4=bk, 8=bq */
    int halfmove_clock;
    int fullmove;
    Move history[MAX_UNDO];
    int history_len;
    int white_captured[7];
    int black_captured[7];
    int move_number;
    char move_notation[MAX_UNDO][8];
    int notation_count;
} Game;

/* Transposition table */
typedef struct {
    uint64_t key;
    int depth;
    int score;
    int flag;   /* 0=exact, 1=lower, 2=upper */
    int best_move;
} TTEntry;

/* ======================================================================
 * PRNG (simple xorshift32)
 * ====================================================================== */

static uint32_t rng_state = 0x12345678;

static uint32_t xorshift32(void)
{
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    return x;
}

/* ======================================================================
 * Utility
 * ====================================================================== */

static int abs_val(int x)
{
    return x < 0 ? -x : x;
}

/* ======================================================================
 * Zobrist hashing
 * ====================================================================== */

static uint64_t zobrist_pieces[64][7][2]; /* [sq][piece_type][color] */
static uint64_t zobrist_castle[16];
static uint64_t zobrist_ep[65];  /* 0-63 = file, 64 = none */
static uint64_t zobrist_side;
static int zobrist_initialized = 0;

static void init_zobrist(void)
{
    int sq, p, c;
    if (zobrist_initialized) return;
    rng_state = 0xA20CA20B;
    for (sq = 0; sq < 64; sq++)
        for (p = 0; p < 7; p++)
            for (c = 0; c < 2; c++)
                zobrist_pieces[sq][p][c] = ((uint64_t)xorshift32() << 32) | xorshift32();
    for (p = 0; p < 16; p++)
        zobrist_castle[p] = ((uint64_t)xorshift32() << 32) | xorshift32();
    for (p = 0; p < 65; p++)
        zobrist_ep[p] = ((uint64_t)xorshift32() << 32) | xorshift32();
    zobrist_side = ((uint64_t)xorshift32() << 32) | xorshift32();
    rng_state = 0xDEADBEEF;
    zobrist_initialized = 1;
}

static uint64_t compute_hash(const Game *g)
{
    uint64_t h = 0;
    int sq;
    for (sq = 0; sq < 64; sq++) {
        int p = g->board[sq];
        if (!IS_EMPTY(p)) {
            int pt = PIECE_TYPE(p);
            int c = IS_WHITE(p) ? 0 : 1;
            h ^= zobrist_pieces[sq][pt][c];
        }
    }
    h ^= zobrist_castle[g->castle & 0xF];
    if (g->ep_square >= 0)
        h ^= zobrist_ep[COL(g->ep_square)];
    else
        h ^= zobrist_ep[64];
    if (g->side == BLACK)
        h ^= zobrist_side;
    return h;
}

/* ======================================================================
 * Transposition Table
 * ====================================================================== */

static TTEntry tt_table[HASH_SIZE];

static int g_difficulty = DIFF_HARD;
static int eval_material, eval_position, eval_king_safety, eval_pawn_structure;
static int eval_mobility, eval_center, eval_threats;

static void tt_init(void)
{
    memset(tt_table, 0, sizeof(tt_table));
}

static int tt_probe(uint64_t key, int depth, int alpha, int beta, int *score, int *best_move)
{
    TTEntry *e = &tt_table[key & (HASH_SIZE - 1)];
    if (e->key == key && e->depth >= depth) {
        *best_move = e->best_move;
        if (e->flag == 0) { /* exact */
            *score = e->score;
            return 1;
        }
        if (e->flag == 1 && e->score >= beta) { /* lower bound */
            *score = e->score;
            return 1;
        }
        if (e->flag == 2 && e->score <= alpha) { /* upper bound */
            *score = e->score;
            return 1;
        }
    }
    *best_move = -1;
    return 0;
}

static void tt_store(uint64_t key, int depth, int score, int flag, int best_move)
{
    TTEntry *e = &tt_table[key & (HASH_SIZE - 1)];
    /* Replace if deeper or different position */
    if (e->key != key || e->depth <= depth) {
        e->key = key;
        e->depth = depth;
        e->score = score;
        e->flag = flag;
        e->best_move = best_move;
    }
}

/* ======================================================================
 * Board initialization
 * ====================================================================== */

static void init_board(Game *g)
{
    static const int setup[64] = {
        ROOK|BLACK, KNIGHT|BLACK, BISHOP|BLACK, QUEEN|BLACK, KING|BLACK, BISHOP|BLACK, KNIGHT|BLACK, ROOK|BLACK,
        PAWN|BLACK, PAWN|BLACK,   PAWN|BLACK,   PAWN|BLACK,  PAWN|BLACK, PAWN|BLACK,   PAWN|BLACK,   PAWN|BLACK,
        0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,
        0,0,0,0,0,0,0,0,
        PAWN|WHITE, PAWN|WHITE,   PAWN|WHITE,   PAWN|WHITE,  PAWN|WHITE, PAWN|WHITE,   PAWN|WHITE,   PAWN|WHITE,
        ROOK|WHITE, KNIGHT|WHITE, BISHOP|WHITE, QUEEN|WHITE, KING|WHITE, BISHOP|WHITE, KNIGHT|WHITE, ROOK|WHITE,
    };
    int i;
    for (i = 0; i < 64; i++)
        g->board[i] = setup[i];
    g->side = WHITE;
    g->ep_square = -1;
    g->castle = 1 | 2 | 4 | 8; /* all rights */
    g->halfmove_clock = 0;
    g->fullmove = 1;
    g->history_len = 0;
}

/* ======================================================================
 * Move generation
 * ====================================================================== */

static int in_bounds(int r, int c)
{
    return r >= 0 && r < 8 && c >= 0 && c < 8;
}

/* Check if square is attacked by the given side */
static int is_attacked(const Game *g, int sq, int by_side)
{
    int r = ROW(sq), c = COL(sq);
    int dr, dc, i, nr, nc;

    /* Knight attacks */
    static const int knight_dr[] = {-2,-2,-1,-1, 1, 1, 2, 2};
    static const int knight_dc[] = {-1, 1,-2, 2,-2, 2,-1, 1};
    for (i = 0; i < 8; i++) {
        nr = r + knight_dr[i];
        nc = c + knight_dc[i];
        if (in_bounds(nr, nc)) {
            int p = g->board[SQ(nr, nc)];
            if (!IS_EMPTY(p) && PIECE_TYPE(p) == KNIGHT && IS_WHITE(p) == (by_side == WHITE))
                return 1;
        }
    }

    /* Pawn attacks */
    if (by_side == WHITE) {
        /* White pawns attack upward (from row+1 to row) */
        if (r + 1 <= 7) {
            if (c - 1 >= 0 && g->board[SQ(r+1, c-1)] == (PAWN | WHITE)) return 1;
            if (c + 1 <= 7 && g->board[SQ(r+1, c+1)] == (PAWN | WHITE)) return 1;
        }
    } else {
        /* Black pawns attack downward (from row-1 to row) */
        if (r - 1 >= 0) {
            if (c - 1 >= 0 && g->board[SQ(r-1, c-1)] == (PAWN | BLACK)) return 1;
            if (c + 1 <= 7 && g->board[SQ(r-1, c+1)] == (PAWN | BLACK)) return 1;
        }
    }

    /* Sliding pieces: bishop/queen (diagonals), rook/queen (straights) */
    static const int diag_dr[] = {-1,-1, 1, 1};
    static const int diag_dc[] = {-1, 1,-1, 1};
    static const int str_dr[] = {-1, 1, 0, 0};
    static const int str_dc[] = { 0, 0,-1, 1};
    int is_diag;

    for (is_diag = 0; is_diag < 2; is_diag++) {
        const int *ddr = is_diag ? diag_dr : str_dr;
        const int *ddc = is_diag ? diag_dc : str_dc;
        for (i = 0; i < 4; i++) {
            dr = ddr[i]; dc = ddc[i];
            nr = r + dr; nc = c + dc;
            while (in_bounds(nr, nc)) {
                int p = g->board[SQ(nr, nc)];
                if (!IS_EMPTY(p)) {
                    if (IS_WHITE(p) == (by_side == WHITE)) {
                        int pt = PIECE_TYPE(p);
                        if (is_diag && (pt == BISHOP || pt == QUEEN)) return 1;
                        if (!is_diag && (pt == ROOK || pt == QUEEN)) return 1;
                    }
                    break;
                }
                nr += dr;
                nc += dc;
            }
        }
    }

    /* King attacks */
    for (dr = -1; dr <= 1; dr++) {
        for (dc = -1; dc <= 1; dc++) {
            if (dr == 0 && dc == 0) continue;
            nr = r + dr; nc = c + dc;
            if (in_bounds(nr, nc)) {
                int p = g->board[SQ(nr, nc)];
                if (!IS_EMPTY(p) && PIECE_TYPE(p) == KING && IS_WHITE(p) == (by_side == WHITE))
                    return 1;
            }
        }
    }

    return 0;
}

/* Find king square */
static int find_king(const Game *g, int side)
{
    int sq;
    int king = KING | side;
    for (sq = 0; sq < 64; sq++)
        if (g->board[sq] == king) return sq;
    return -1;
}

static int in_check(const Game *g, int side)
{
    int ksq = find_king(g, side);
    if (ksq < 0) return 0;
    return is_attacked(g, ksq, side == WHITE ? BLACK : WHITE);
}

/* Generate pseudo-legal moves (may leave king in check) */
static int gen_pseudo_legal(const Game *g, Move *moves)
{
    int count = 0;
    int sq, r, c, nr, nc, i, dr, dc;
    int side = g->side;
    int enemy = side == WHITE ? BLACK : WHITE;

    for (sq = 0; sq < 64; sq++) {
        int p = g->board[sq];
        if (IS_EMPTY(p) || PIECE_COLOR(p) != side) continue;
        int pt = PIECE_TYPE(p);
        r = ROW(sq); c = COL(sq);

        switch (pt) {
        case PAWN: {
            int dir = (side == WHITE) ? -1 : 1;
            int start_row = (side == WHITE) ? 6 : 1;
            int promo_row = (side == WHITE) ? 0 : 7;

            /* Forward */
            nr = r + dir;
            if (in_bounds(nr, c) && IS_EMPTY(g->board[SQ(nr, c)])) {
                if (nr == promo_row) {
                    moves[count].from = sq;
                    moves[count].to = SQ(nr, c);
                    moves[count].piece = p;
                    moves[count].captured = EMPTY;
                    moves[count].promoted = QUEEN;
                    moves[count].castle = 0;
                    moves[count].en_passant = 0;
                    moves[count].prev_ep = g->ep_square;
                    moves[count].prev_castle = g->castle;
                    moves[count].halfmove = g->halfmove_clock;
                    count++;
                    moves[count].from = sq;
                    moves[count].to = SQ(nr, c);
                    moves[count].piece = p;
                    moves[count].captured = EMPTY;
                    moves[count].promoted = ROOK;
                    moves[count].castle = 0;
                    moves[count].en_passant = 0;
                    moves[count].prev_ep = g->ep_square;
                    moves[count].prev_castle = g->castle;
                    moves[count].halfmove = g->halfmove_clock;
                    count++;
                    moves[count].from = sq;
                    moves[count].to = SQ(nr, c);
                    moves[count].piece = p;
                    moves[count].captured = EMPTY;
                    moves[count].promoted = BISHOP;
                    moves[count].castle = 0;
                    moves[count].en_passant = 0;
                    moves[count].prev_ep = g->ep_square;
                    moves[count].prev_castle = g->castle;
                    moves[count].halfmove = g->halfmove_clock;
                    count++;
                    moves[count].from = sq;
                    moves[count].to = SQ(nr, c);
                    moves[count].piece = p;
                    moves[count].captured = EMPTY;
                    moves[count].promoted = KNIGHT;
                    moves[count].castle = 0;
                    moves[count].en_passant = 0;
                    moves[count].prev_ep = g->ep_square;
                    moves[count].prev_castle = g->castle;
                    moves[count].halfmove = g->halfmove_clock;
                    count++;
                } else {
                    moves[count].from = sq;
                    moves[count].to = SQ(nr, c);
                    moves[count].piece = p;
                    moves[count].captured = EMPTY;
                    moves[count].promoted = EMPTY;
                    moves[count].castle = 0;
                    moves[count].en_passant = 0;
                    moves[count].prev_ep = g->ep_square;
                    moves[count].prev_castle = g->castle;
                    moves[count].halfmove = g->halfmove_clock;
                    count++;
                }
                /* Double push */
                if (r == start_row) {
                    nr = r + 2 * dir;
                    if (in_bounds(nr, c) && IS_EMPTY(g->board[SQ(nr, c)])) {
                        moves[count].from = sq;
                        moves[count].to = SQ(nr, c);
                        moves[count].piece = p;
                        moves[count].captured = EMPTY;
                        moves[count].promoted = EMPTY;
                        moves[count].castle = 0;
                        moves[count].en_passant = 0;
                        moves[count].prev_ep = g->ep_square;
                        moves[count].prev_castle = g->castle;
                        moves[count].halfmove = g->halfmove_clock;
                        count++;
                    }
                }
            }

            /* Captures */
            for (dc = -1; dc <= 1; dc += 2) {
                nc = c + dc;
                nr = r + dir;
                if (!in_bounds(nr, nc)) continue;
                int target = g->board[SQ(nr, nc)];
                int is_ep = (g->ep_square == SQ(nr, nc));
                if ((!IS_EMPTY(target) && PIECE_COLOR(target) == enemy) || is_ep) {
                    int cap = is_ep ? (PAWN | enemy) : target;
                    if (nr == promo_row) {
                        int promos[] = {QUEEN, ROOK, BISHOP, KNIGHT};
                        int pi;
                        for (pi = 0; pi < 4; pi++) {
                            moves[count].from = sq;
                            moves[count].to = SQ(nr, nc);
                            moves[count].piece = p;
                            moves[count].captured = cap;
                            moves[count].promoted = promos[pi];
                            moves[count].castle = 0;
                            moves[count].en_passant = is_ep;
                            moves[count].prev_ep = g->ep_square;
                            moves[count].prev_castle = g->castle;
                            moves[count].halfmove = g->halfmove_clock;
                            count++;
                        }
                    } else {
                        moves[count].from = sq;
                        moves[count].to = SQ(nr, nc);
                        moves[count].piece = p;
                        moves[count].captured = cap;
                        moves[count].promoted = EMPTY;
                        moves[count].castle = 0;
                        moves[count].en_passant = is_ep;
                        moves[count].prev_ep = g->ep_square;
                        moves[count].prev_castle = g->castle;
                        moves[count].halfmove = g->halfmove_clock;
                        count++;
                    }
                }
            }
            break;
        }

        case KNIGHT: {
            static const int ndr[] = {-2,-2,-1,-1, 1, 1, 2, 2};
            static const int ndc[] = {-1, 1,-2, 2,-2, 2,-1, 1};
            for (i = 0; i < 8; i++) {
                nr = r + ndr[i]; nc = c + ndc[i];
                if (!in_bounds(nr, nc)) continue;
                int target = g->board[SQ(nr, nc)];
                if (!IS_EMPTY(target) && PIECE_COLOR(target) == side) continue;
                moves[count].from = sq;
                moves[count].to = SQ(nr, nc);
                moves[count].piece = p;
                moves[count].captured = target;
                moves[count].promoted = EMPTY;
                moves[count].castle = 0;
                moves[count].en_passant = 0;
                moves[count].prev_ep = g->ep_square;
                moves[count].prev_castle = g->castle;
                moves[count].halfmove = g->halfmove_clock;
                count++;
            }
            break;
        }

        case BISHOP: case ROOK: case QUEEN: {
            /* Diagonals (0-3), straights (4-7) */
            static const int sdr[] = {-1,-1, 1, 1, -1, 0, 0, 1};
            static const int sdc[] = {-1, 1,-1, 1,  0,-1, 1, 0};
            int start = (pt == BISHOP) ? 0 : (pt == ROOK) ? 4 : 0;
            int end = (pt == BISHOP) ? 4 : (pt == ROOK) ? 8 : 8;
            for (i = start; i < end; i++) {
                dr = sdr[i]; dc = sdc[i];
                nr = r + dr; nc = c + dc;
                while (in_bounds(nr, nc)) {
                    int target = g->board[SQ(nr, nc)];
                    if (!IS_EMPTY(target)) {
                        if (PIECE_COLOR(target) != side) {
                            moves[count].from = sq;
                            moves[count].to = SQ(nr, nc);
                            moves[count].piece = p;
                            moves[count].captured = target;
                            moves[count].promoted = EMPTY;
                            moves[count].castle = 0;
                            moves[count].en_passant = 0;
                            moves[count].prev_ep = g->ep_square;
                            moves[count].prev_castle = g->castle;
                            moves[count].halfmove = g->halfmove_clock;
                            count++;
                        }
                        break;
                    }
                    moves[count].from = sq;
                    moves[count].to = SQ(nr, nc);
                    moves[count].piece = p;
                    moves[count].captured = EMPTY;
                    moves[count].promoted = EMPTY;
                    moves[count].castle = 0;
                    moves[count].en_passant = 0;
                    moves[count].prev_ep = g->ep_square;
                    moves[count].prev_castle = g->castle;
                    moves[count].halfmove = g->halfmove_clock;
                    count++;
                    nr += dr; nc += dc;
                }
            }
            break;
        }

        case KING: {
            static const int kdr[] = {-1,-1,-1, 0, 0, 1, 1, 1};
            static const int kdc[] = {-1, 0, 1,-1, 1,-1, 0, 1};
            for (i = 0; i < 8; i++) {
                nr = r + kdr[i]; nc = c + kdc[i];
                if (!in_bounds(nr, nc)) continue;
                int target = g->board[SQ(nr, nc)];
                if (!IS_EMPTY(target) && PIECE_COLOR(target) == side) continue;
                moves[count].from = sq;
                moves[count].to = SQ(nr, nc);
                moves[count].piece = p;
                moves[count].captured = target;
                moves[count].promoted = EMPTY;
                moves[count].castle = 0;
                moves[count].en_passant = 0;
                moves[count].prev_ep = g->ep_square;
                moves[count].prev_castle = g->castle;
                moves[count].halfmove = g->halfmove_clock;
                count++;
            }

            /* Castling */
            if (side == WHITE && r == 7 && c == 4) {
                /* Kingside */
                if ((g->castle & 1) && IS_EMPTY(g->board[SQ(7,5)]) &&
                    IS_EMPTY(g->board[SQ(7,6)]) &&
                    g->board[SQ(7,7)] == (ROOK|WHITE) &&
                    !is_attacked(g, SQ(7,4), BLACK) &&
                    !is_attacked(g, SQ(7,5), BLACK) &&
                    !is_attacked(g, SQ(7,6), BLACK)) {
                    moves[count].from = sq;
                    moves[count].to = SQ(7, 6);
                    moves[count].piece = p;
                    moves[count].captured = EMPTY;
                    moves[count].promoted = EMPTY;
                    moves[count].castle = 1;
                    moves[count].en_passant = 0;
                    moves[count].prev_ep = g->ep_square;
                    moves[count].prev_castle = g->castle;
                    moves[count].halfmove = g->halfmove_clock;
                    count++;
                }
                /* Queenside */
                if ((g->castle & 2) && IS_EMPTY(g->board[SQ(7,3)]) &&
                    IS_EMPTY(g->board[SQ(7,2)]) &&
                    IS_EMPTY(g->board[SQ(7,1)]) &&
                    g->board[SQ(7,0)] == (ROOK|WHITE) &&
                    !is_attacked(g, SQ(7,4), BLACK) &&
                    !is_attacked(g, SQ(7,3), BLACK) &&
                    !is_attacked(g, SQ(7,2), BLACK)) {
                    moves[count].from = sq;
                    moves[count].to = SQ(7, 2);
                    moves[count].piece = p;
                    moves[count].captured = EMPTY;
                    moves[count].promoted = EMPTY;
                    moves[count].castle = 2;
                    moves[count].en_passant = 0;
                    moves[count].prev_ep = g->ep_square;
                    moves[count].prev_castle = g->castle;
                    moves[count].halfmove = g->halfmove_clock;
                    count++;
                }
            }
            if (side == BLACK && r == 0 && c == 4) {
                /* Kingside */
                if ((g->castle & 4) && IS_EMPTY(g->board[SQ(0,5)]) &&
                    IS_EMPTY(g->board[SQ(0,6)]) &&
                    g->board[SQ(0,7)] == (ROOK|BLACK) &&
                    !is_attacked(g, SQ(0,4), WHITE) &&
                    !is_attacked(g, SQ(0,5), WHITE) &&
                    !is_attacked(g, SQ(0,6), WHITE)) {
                    moves[count].from = sq;
                    moves[count].to = SQ(0, 6);
                    moves[count].piece = p;
                    moves[count].captured = EMPTY;
                    moves[count].promoted = EMPTY;
                    moves[count].castle = 1;
                    moves[count].en_passant = 0;
                    moves[count].prev_ep = g->ep_square;
                    moves[count].prev_castle = g->castle;
                    moves[count].halfmove = g->halfmove_clock;
                    count++;
                }
                /* Queenside */
                if ((g->castle & 8) && IS_EMPTY(g->board[SQ(0,3)]) &&
                    IS_EMPTY(g->board[SQ(0,2)]) &&
                    IS_EMPTY(g->board[SQ(0,1)]) &&
                    g->board[SQ(0,0)] == (ROOK|BLACK) &&
                    !is_attacked(g, SQ(0,4), WHITE) &&
                    !is_attacked(g, SQ(0,3), WHITE) &&
                    !is_attacked(g, SQ(0,2), WHITE)) {
                    moves[count].from = sq;
                    moves[count].to = SQ(0, 2);
                    moves[count].piece = p;
                    moves[count].captured = EMPTY;
                    moves[count].promoted = EMPTY;
                    moves[count].castle = 2;
                    moves[count].en_passant = 0;
                    moves[count].prev_ep = g->ep_square;
                    moves[count].prev_castle = g->castle;
                    moves[count].halfmove = g->halfmove_clock;
                    count++;
                }
            }
            break;
        }
        }
    }

    return count;
}

/* ======================================================================
 * Make / Unmake move
 * ====================================================================== */

static void make_move(Game *g, const Move *m)
{
    int from = m->from;
    int to = m->to;

    g->history[g->history_len] = *m;
    g->history_len++;

    /* Handle castling rook movement */
    if (m->castle) {
        if (m->castle == 1) { /* kingside */
            if (IS_WHITE(m->piece)) {
                g->board[SQ(7,5)] = g->board[SQ(7,7)];
                g->board[SQ(7,7)] = EMPTY;
            } else {
                g->board[SQ(0,5)] = g->board[SQ(0,7)];
                g->board[SQ(0,7)] = EMPTY;
            }
        } else { /* queenside */
            if (IS_WHITE(m->piece)) {
                g->board[SQ(7,3)] = g->board[SQ(7,0)];
                g->board[SQ(7,0)] = EMPTY;
            } else {
                g->board[SQ(0,3)] = g->board[SQ(0,0)];
                g->board[SQ(0,0)] = EMPTY;
            }
        }
    }

    /* Handle en passant capture */
    if (m->en_passant) {
        int cap_sq = ROW(from) * 8 + COL(to); /* captured pawn is on same row as capturer */
        int cap_piece = g->board[cap_sq];
        g->board[cap_sq] = EMPTY;
        if (!IS_EMPTY(cap_piece)) {
            int cap_type = PIECE_TYPE(cap_piece);
            if (IS_WHITE(cap_piece)) g->white_captured[cap_type]++;
            else g->black_captured[cap_type]++;
        }
    }

    if (!IS_EMPTY(m->captured) && !m->en_passant) {
        int cap_type = PIECE_TYPE(m->captured);
        if (IS_WHITE(m->captured)) g->white_captured[cap_type]++;
        else g->black_captured[cap_type]++;
    }

    /* Move piece */
    g->board[to] = m->piece;
    g->board[from] = EMPTY;

    /* Handle promotion */
    if (m->promoted != EMPTY) {
        int color = PIECE_COLOR(m->piece);
        g->board[to] = PIECE(m->promoted, color);
    }

    /* Update en passant square */
    if (PIECE_TYPE(m->piece) == PAWN && abs_val(ROW(to) - ROW(from)) == 2) {
        g->ep_square = (ROW(from) + ROW(to)) / 2 * 8 + COL(from);
    } else {
        g->ep_square = -1;
    }

    /* Update castling rights */
    if (PIECE_TYPE(m->piece) == KING) {
        if (IS_WHITE(m->piece)) g->castle &= ~(1 | 2);
        else g->castle &= ~(4 | 8);
    }
    if (PIECE_TYPE(m->piece) == ROOK) {
        if (from == SQ(7,0)) g->castle &= ~2;
        if (from == SQ(7,7)) g->castle &= ~1;
        if (from == SQ(0,0)) g->castle &= ~8;
        if (from == SQ(0,7)) g->castle &= ~4;
    }
    /* If rook captured */
    if (to == SQ(7,0)) g->castle &= ~2;
    if (to == SQ(7,7)) g->castle &= ~1;
    if (to == SQ(0,0)) g->castle &= ~8;
    if (to == SQ(0,7)) g->castle &= ~4;

    /* Update halfmove clock */
    if (PIECE_TYPE(m->piece) == PAWN || !IS_EMPTY(m->captured))
        g->halfmove_clock = 0;
    else
        g->halfmove_clock++;

    /* Switch side */
    g->side = (g->side == WHITE) ? BLACK : WHITE;
    if (g->side == WHITE) g->fullmove++;
}

static void unmake_move(Game *g)
{
    Move m;
    int from, to;

    if (g->history_len <= 0) return;
    g->history_len--;
    m = g->history[g->history_len];

    from = m.from;
    to = m.to;

    /* Switch side back */
    g->side = (g->side == WHITE) ? BLACK : WHITE;
    if (g->side == BLACK) g->fullmove--;

    /* Move piece back */
    g->board[from] = m.piece;
    g->board[to] = m.captured;

    /* Handle promotion */
    if (m.promoted != EMPTY) {
        g->board[from] = m.piece; /* restore original pawn */
    }

    /* Handle en passant restore */
    if (m.en_passant) {
        int cap_sq = ROW(from) * 8 + COL(to);
        int enemy = IS_WHITE(m.piece) ? BLACK : WHITE;
        g->board[cap_sq] = PAWN | enemy;
        g->board[to] = EMPTY; /* the target square was empty */
    }

    /* Handle castling rook restore */
    if (m.castle) {
        if (m.castle == 1) { /* kingside */
            if (IS_WHITE(m.piece)) {
                g->board[SQ(7,7)] = g->board[SQ(7,5)];
                g->board[SQ(7,5)] = EMPTY;
            } else {
                g->board[SQ(0,7)] = g->board[SQ(0,5)];
                g->board[SQ(0,5)] = EMPTY;
            }
        } else { /* queenside */
            if (IS_WHITE(m.piece)) {
                g->board[SQ(7,0)] = g->board[SQ(7,3)];
                g->board[SQ(7,3)] = EMPTY;
            } else {
                g->board[SQ(0,0)] = g->board[SQ(0,3)];
                g->board[SQ(0,3)] = EMPTY;
            }
        }
    }

    /* Restore state */
    g->ep_square = m.prev_ep;
    g->castle = m.prev_castle;
    g->halfmove_clock = m.halfmove;
}

/* Generate legal moves */
static int gen_legal(const Game *g, Move *moves)
{
    Move pseudo[MAX_MOVES];
    int count = gen_pseudo_legal(g, pseudo);
    int legal = 0;
    int i;
    Game copy;

    for (i = 0; i < count; i++) {
        copy = *g;
        make_move(&copy, &pseudo[i]);
        /* After make_move, side switched; check if our king is in check */
        if (!in_check(&copy, g->side)) {
            moves[legal++] = pseudo[i];
        }
    }
    return legal;
}

/* ======================================================================
 * Evaluation
 * ====================================================================== */

/* Piece-square tables (from White's perspective, row 0 = rank 8) */
static const int pst_pawn[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
    50, 50, 50, 50, 50, 50, 50, 50,
    10, 10, 20, 30, 30, 20, 10, 10,
     5,  5, 10, 25, 25, 10,  5,  5,
     0,  0,  0, 20, 20,  0,  0,  0,
     5, -5,-10,  0,  0,-10, -5,  5,
     5, 10, 10,-20,-20, 10, 10,  5,
     0,  0,  0,  0,  0,  0,  0,  0
};

static const int pst_knight[64] = {
    -50,-40,-30,-30,-30,-30,-40,-50,
    -40,-20,  0,  0,  0,  0,-20,-40,
    -30,  0, 10, 15, 15, 10,  0,-30,
    -30,  5, 15, 20, 20, 15,  5,-30,
    -30,  0, 15, 20, 20, 15,  0,-30,
    -30,  5, 10, 15, 15, 10,  5,-30,
    -40,-20,  0,  5,  5,  0,-20,-40,
    -50,-40,-30,-30,-30,-30,-40,-50,
};

static const int pst_bishop[64] = {
    -20,-10,-10,-10,-10,-10,-10,-20,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -10,  0, 10, 10, 10, 10,  0,-10,
    -10,  5,  5, 10, 10,  5,  5,-10,
    -10,  0, 10, 10, 10, 10,  0,-10,
    -10, 10, 10, 10, 10, 10, 10,-10,
    -10,  5,  0,  0,  0,  0,  5,-10,
    -20,-10,-10,-10,-10,-10,-10,-20,
};

static const int pst_rook[64] = {
     0,  0,  0,  0,  0,  0,  0,  0,
     5, 10, 10, 10, 10, 10, 10,  5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
    -5,  0,  0,  0,  0,  0,  0, -5,
     0,  0,  0,  5,  5,  0,  0,  0
};

static const int pst_queen[64] = {
    -20,-10,-10, -5, -5,-10,-10,-20,
    -10,  0,  0,  0,  0,  0,  0,-10,
    -10,  0,  5,  5,  5,  5,  0,-10,
     -5,  0,  5,  5,  5,  5,  0, -5,
      0,  0,  5,  5,  5,  5,  0, -5,
    -10,  5,  5,  5,  5,  5,  0,-10,
    -10,  0,  5,  0,  0,  0,  0,-10,
    -20,-10,-10, -5, -5,-10,-10,-20,
};

static const int pst_king_middle[64] = {
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -30,-40,-40,-50,-50,-40,-40,-30,
    -20,-30,-30,-40,-40,-30,-30,-20,
    -10,-20,-20,-20,-20,-20,-20,-10,
     20, 20,  0,  0,  0,  0, 20, 20,
     20, 30, 10,  0,  0, 10, 30, 20
};

static int count_pawn(const Game *g, int color, int file)
{
    int r, count = 0;
    for (r = 0; r < 8; r++) {
        int p = g->board[SQ(r, file)];
        if (p == (PAWN | (color ? BLACK : WHITE))) count++;
    }
    return count;
}

static int has_pawn_on_file(const Game *g, int color, int file)
{
    return count_pawn(g, color, file) > 0;
}

static int is_passed_pawn(const Game *g, int sq, int color)
{
    int r = ROW(sq), c = COL(sq);
    int i;
    int enemy_pawn = PAWN | (color ? BLACK : WHITE);
    (void)enemy_pawn;

    if (color == 0) {
        for (i = 0; i < r; i++) {
            if (c - 1 >= 0 && g->board[SQ(i, c - 1)] == (PAWN | BLACK)) return 0;
            if (g->board[SQ(i, c)] == (PAWN | BLACK)) return 0;
            if (c + 1 <= 7 && g->board[SQ(i, c + 1)] == (PAWN | BLACK)) return 0;
        }
    } else {
        for (i = r + 1; i < 8; i++) {
            if (c - 1 >= 0 && g->board[SQ(i, c - 1)] == (PAWN | WHITE)) return 0;
            if (g->board[SQ(i, c)] == (PAWN | WHITE)) return 0;
            if (c + 1 <= 7 && g->board[SQ(i, c + 1)] == (PAWN | WHITE)) return 0;
        }
    }
    return 1;
}

static int king_safety(const Game *g, int color)
{
    int king_sq = -1;
    int sq, r, c, nr, nc, i;
    int penalty = 0;
    int king = KING | (color ? BLACK : WHITE);
    int enemy = color ? WHITE : BLACK;

    for (sq = 0; sq < 64; sq++)
        if (g->board[sq] == king) { king_sq = sq; break; }
    if (king_sq < 0) return 0;

    r = ROW(king_sq); c = COL(king_sq);

    static const int knight_dr[] = {-2,-2,-1,-1, 1, 1, 2, 2};
    static const int knight_dc[] = {-1, 1,-2, 2,-2, 2,-1, 1};

    for (nr = r - 2; nr <= r + 2; nr++) {
        for (nc = c - 2; nc <= c + 2; nc++) {
            if (!in_bounds(nr, nc)) continue;
            int p = g->board[SQ(nr, nc)];
            if (IS_EMPTY(p) || PIECE_COLOR(p) != (enemy ? BLACK : WHITE)) continue;
            int pt = PIECE_TYPE(p);
            int dist = abs_val(nr - r) + abs_val(nc - c);
            if (pt == QUEEN) penalty += (dist <= 2) ? 20 : 10;
            else if (pt == ROOK) penalty += (dist <= 2) ? 10 : 5;
            else if (pt == BISHOP) penalty += (dist <= 2) ? 8 : 4;
            else if (pt == KNIGHT) penalty += (dist <= 1) ? 15 : 5;
        }
    }

    for (i = 0; i < 8; i++) {
        nr = r + knight_dr[i]; nc = c + knight_dc[i];
        if (in_bounds(nr, nc)) {
            int p = g->board[SQ(nr, nc)];
            if (!IS_EMPTY(p) && PIECE_TYPE(p) == KNIGHT &&
                PIECE_COLOR(p) == (enemy ? BLACK : WHITE)) {
                penalty += 15;
            }
        }
    }

    int pawn_dir = color ? -1 : 1;
    int shield_count = 0;
    for (nc = c - 1; nc <= c + 1; nc++) {
        if (!in_bounds(r + pawn_dir, nc)) continue;
        int p = g->board[SQ(r + pawn_dir, nc)];
        if (p == (PAWN | (color ? BLACK : WHITE))) shield_count++;
    }
    penalty -= shield_count * 8;

    int open_files_near = 0;
    for (nc = c - 1; nc <= c + 1; nc++) {
        if (!in_bounds(0, nc)) continue;
        if (!has_pawn_on_file(g, color, nc)) open_files_near++;
    }
    penalty += open_files_near * 10;

    return penalty;
}

static int evaluate(const Game *g)
{
    int score = 0;
    int sq, p, pt, c;
    int white_pawns_on_file[8] = {0};
    int black_pawns_on_file[8] = {0};
    int white_pawn_count = 0, black_pawn_count = 0;

    eval_material = 0;
    eval_position = 0;
    eval_king_safety = 0;
    eval_pawn_structure = 0;
    eval_mobility = 0;
    eval_center = 0;
    eval_threats = 0;

    for (sq = 0; sq < 64; sq++) {
        p = g->board[sq];
        if (IS_EMPTY(p)) continue;
        pt = PIECE_TYPE(p);
        c = IS_WHITE(p) ? 0 : 1;
        int sign = c == 0 ? 1 : -1;

        switch (pt) {
        case PAWN:
            eval_material += sign * VAL_PAWN;
            eval_position += sign * (c == 0 ? pst_pawn[sq] : pst_pawn[sq ^ 56]);
            if (c == 0) { white_pawns_on_file[COL(sq)]++; white_pawn_count++; }
            else { black_pawns_on_file[COL(sq)]++; black_pawn_count++; }
            break;
        case KNIGHT:
            eval_material += sign * VAL_KNIGHT;
            eval_position += sign * (c == 0 ? pst_knight[sq] : pst_knight[sq ^ 56]);
            break;
        case BISHOP:
            eval_material += sign * VAL_BISHOP;
            eval_position += sign * (c == 0 ? pst_bishop[sq] : pst_bishop[sq ^ 56]);
            break;
        case ROOK:
            eval_material += sign * VAL_ROOK;
            eval_position += sign * (c == 0 ? pst_rook[sq] : pst_rook[sq ^ 56]);
            break;
        case QUEEN:
            eval_material += sign * VAL_QUEEN;
            eval_position += sign * (c == 0 ? pst_queen[sq] : pst_queen[sq ^ 56]);
            break;
        case KING:
            eval_position += sign * (c == 0 ? pst_king_middle[sq] : pst_king_middle[sq ^ 56]);
            break;
        }
    }
    score = eval_material + eval_position;

    if (g_difficulty >= DIFF_MEDIUM) {
        for (sq = 0; sq < 64; sq++) {
            p = g->board[sq];
            if (IS_EMPTY(p) || PIECE_TYPE(p) != PAWN) continue;
            c = IS_WHITE(p) ? 0 : 1;
            int file = COL(sq);

            if (c == 0) {
                if (white_pawns_on_file[file] > 1) eval_pawn_structure -= 15;
                int left_ok = (file > 0) ? has_pawn_on_file(g, 0, file - 1) : 0;
                int right_ok = (file < 7) ? has_pawn_on_file(g, 0, file + 1) : 0;
                if (!left_ok && !right_ok) eval_pawn_structure -= 20;
                else if (!left_ok || !right_ok) eval_pawn_structure -= 8;
                if (is_passed_pawn(g, sq, 0)) {
                    int adv = ROW(sq);
                    eval_pawn_structure += 10 + adv * adv * 5;
                }
            } else {
                if (black_pawns_on_file[file] > 1) eval_pawn_structure += 15;
                int left_ok = (file > 0) ? has_pawn_on_file(g, 1, file - 1) : 0;
                int right_ok = (file < 7) ? has_pawn_on_file(g, 1, file + 1) : 0;
                if (!left_ok && !right_ok) eval_pawn_structure += 20;
                else if (!left_ok || !right_ok) eval_pawn_structure += 8;
                if (is_passed_pawn(g, sq, 1)) {
                    int adv = 7 - ROW(sq);
                    eval_pawn_structure -= 10 + adv * adv * 5;
                }
            }
        }
        score += eval_pawn_structure;
    }

    if (g_difficulty >= DIFF_MEDIUM) {
        eval_king_safety = -king_safety(g, 0) + king_safety(g, 1);
        score += eval_king_safety;
    }

    if (g_difficulty >= DIFF_HARD) {
        int center_bonus_arr[4] = {3, 5, 5, 3};
        static const int center_sq_arr[4][2] = {{3,3},{3,4},{4,3},{4,4}};
        for (sq = 0; sq < 64; sq++) {
            p = g->board[sq];
            if (IS_EMPTY(p)) continue;
            c = IS_WHITE(p) ? 0 : 1;
            int sign = c == 0 ? 1 : -1;
            int r = ROW(sq), col = COL(sq);
            for (int k = 0; k < 4; k++) {
                if (r == center_sq_arr[k][0] && col == center_sq_arr[k][1]) {
                    eval_center += sign * center_bonus_arr[k];
                    break;
                }
            }
        }
        score += eval_center;
    }

    {
        Move moves[MAX_MOVES];
        int mobility;
        mobility = gen_legal(g, moves);
        eval_mobility = (g->side == WHITE ? 1 : -1) * mobility * 3;
        Game copy = *g;
        copy.side = (g->side == WHITE) ? BLACK : WHITE;
        mobility = gen_legal(&copy, moves);
        eval_mobility += (g->side == WHITE ? -1 : 1) * mobility * 3;
        score += eval_mobility;
    }

    if (g_difficulty >= DIFF_HARD) {
        for (sq = 0; sq < 64; sq++) {
            p = g->board[sq];
            if (IS_EMPTY(p)) continue;
            pt = PIECE_TYPE(p);
            c = IS_WHITE(p) ? 0 : 1;
            int sign = c == 0 ? 1 : -1;
            int r = ROW(sq), col = COL(sq);
            int dr, dc, nr, nc, i;

            if (pt == PAWN) {
                int pawn_dir = c == 0 ? -1 : 1;
                for (dc = -1; dc <= 1; dc += 2) {
                    nr = r + pawn_dir; nc = col + dc;
                    if (!in_bounds(nr, nc)) continue;
                    int target = g->board[SQ(nr, nc)];
                    if (!IS_EMPTY(target) && PIECE_COLOR(target) != (c == 0 ? WHITE : BLACK)) {
                        int tp = PIECE_TYPE(target);
                        if (tp == KNIGHT || tp == BISHOP) eval_threats += sign * 15;
                        else if (tp == ROOK) eval_threats += sign * 25;
                        else if (tp == QUEEN) eval_threats += sign * 40;
                    }
                }
            }
            else if (pt == KNIGHT || pt == BISHOP) {
                int attack_val = (pt == KNIGHT) ? VAL_KNIGHT : VAL_BISHOP;
                static const int kdr[] = {-2,-2,-1,-1,1,1,2,2};
                static const int kdc[] = {-1,1,-2,2,-2,2,-1,1};
                static const int bdr[] = {-1,-1,1,1};
                static const int bdc[] = {-1,1,-1,1};
                if (pt == KNIGHT) {
                    for (i = 0; i < 8; i++) {
                        nr = r + kdr[i]; nc = col + kdc[i];
                        if (!in_bounds(nr, nc)) continue;
                        int target = g->board[SQ(nr, nc)];
                        if (!IS_EMPTY(target) && PIECE_COLOR(target) != (c == 0 ? WHITE : BLACK)) {
                            int tp = PIECE_TYPE(target);
                            int tval = (tp == PAWN) ? VAL_PAWN : (tp == KNIGHT) ? VAL_KNIGHT :
                                       (tp == BISHOP) ? VAL_BISHOP : (tp == ROOK) ? VAL_ROOK :
                                       (tp == QUEEN) ? VAL_QUEEN : 0;
                            if (tval > attack_val) eval_threats += sign * 10;
                        }
                    }
                } else {
                    for (i = 0; i < 4; i++) {
                        dr = bdr[i]; dc = bdc[i];
                        nr = r + dr; nc = col + dc;
                        while (in_bounds(nr, nc)) {
                            int target = g->board[SQ(nr, nc)];
                            if (!IS_EMPTY(target)) {
                                if (PIECE_COLOR(target) != (c == 0 ? WHITE : BLACK)) {
                                    int tp = PIECE_TYPE(target);
                                    int tval = (tp == PAWN) ? VAL_PAWN : (tp == KNIGHT) ? VAL_KNIGHT :
                                               (tp == BISHOP) ? VAL_BISHOP : (tp == ROOK) ? VAL_ROOK :
                                               (tp == QUEEN) ? VAL_QUEEN : 0;
                                    if (tval > attack_val) eval_threats += sign * 10;
                                }
                                break;
                            }
                            nr += dr; nc += dc;
                        }
                    }
                }
            }
        }
        score += eval_threats;
    }

    return score;
}

/* ======================================================================
 * AI Engine — Minimax with Alpha-Beta
 * ====================================================================== */

/* MVV-LVA ordering */
static int mvv_lva(const Move *m)
{
    static const int piece_value[] = {0, 1, 3, 3, 5, 9, 100};
    int victim = PIECE_TYPE(m->captured);
    int attacker = PIECE_TYPE(m->piece);
    if (victim == 0) return 0;
    return piece_value[victim] * 10 - piece_value[attacker];
}

static void order_moves(Move *moves, int count, int tt_move)
{
    int i;
    int scores[MAX_MOVES];

    for (i = 0; i < count; i++) {
        scores[i] = 0;
        if (moves[i].from == (tt_move & 0x3F) && moves[i].to == ((tt_move >> 6) & 0x3F))
            scores[i] += 1000000;
        scores[i] += mvv_lva(&moves[i]) * 10;
        if (moves[i].promoted != EMPTY) scores[i] += 500;
        if (moves[i].castle) scores[i] += 200;
        /* Center control */
        {
            int r = ROW(moves[i].to), c = COL(moves[i].to);
            if (r >= 2 && r <= 5 && c >= 2 && c <= 5) scores[i] += 10;
            if (r >= 3 && r <= 4 && c >= 3 && c <= 4) scores[i] += 20;
        }
    }

    /* Simple selection sort by score */
    for (i = 0; i < count - 1; i++) {
        int j, best = i;
        for (j = i + 1; j < count; j++)
            if (scores[j] > scores[best]) best = j;
        if (best != i) {
            Move tm = moves[i]; moves[i] = moves[best]; moves[best] = tm;
            int ts = scores[i]; scores[i] = scores[best]; scores[best] = ts;
        }
    }
}

static int quiesce(Game *g, int alpha, int beta, int depth)
{
    int stand_pat = evaluate(g);
    Move moves[MAX_MOVES];
    int count, i;

    if (stand_pat >= beta) return beta;
    if (alpha < stand_pat) alpha = stand_pat;
    if (depth <= 0) return alpha;

    count = gen_legal(g, moves);

    /* Only consider captures in quiescence */
    for (i = 0; i < count; i++) {
        if (IS_EMPTY(moves[i].captured)) continue;

        make_move(g, &moves[i]);
        int score = -quiesce(g, -beta, -alpha, depth - 1);
        unmake_move(g);

        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }

    return alpha;
}

static int alpha_beta(Game *g, int depth, int alpha, int beta, uint64_t key, int do_null)
{
    Move moves[MAX_MOVES];
    int count, i;
    int score;
    int tt_move = -1;

    /* Terminal conditions */
    count = gen_legal(g, moves);
    if (count == 0) {
        if (in_check(g, g->side))
            return -INF - depth; /* checkmate (prefer faster mate) */
        return 0; /* stalemate */
    }

    if (g->halfmove_clock >= 100) return 0; /* 50-move rule */

    /* Mate distance pruning */
    if (alpha < -INF) alpha = -INF;
    if (beta > INF) beta = INF;
    if (alpha >= beta) return alpha;

    /* TT probe */
    if (tt_probe(key, depth, alpha, beta, &score, &tt_move)) {
        return score;
    }

    if (depth <= 0) {
        return quiesce(g, alpha, beta, 6);
    }

    /* Null move pruning (skip if in check) */
    if (do_null && depth >= 3 && !in_check(g, g->side)) {
        int R = 3;
        Game copy = *g;
        copy.ep_square = -1;
        copy.side = (copy.side == WHITE) ? BLACK : WHITE;
        uint64_t nkey = compute_hash(&copy);
        score = -alpha_beta(&copy, depth - R - 1, -beta, -beta + 1, nkey, 0);
        if (score >= beta) return beta;
    }

    /* IID if no TT move */
    if (tt_move < 0 && depth >= 3) {
        alpha_beta(g, depth - 2, alpha, beta, key, do_null);
        tt_probe(key, depth - 2, alpha, beta, &score, &tt_move);
    }

    order_moves(moves, count, tt_move);

    int best_score = -INF - 10;
    int best_move = -1;
    int flag = 2; /* upper bound */

    for (i = 0; i < count; i++) {
        make_move(g, &moves[i]);
        uint64_t nkey = key ^ zobrist_side;
        if (g->ep_square >= 0)
            nkey ^= zobrist_ep[COL(g->ep_square)];

        /* Late move reduction */
        int new_depth = depth - 1;
        if (i >= 4 && depth >= 3 &&
            IS_EMPTY(moves[i].captured) && moves[i].promoted == EMPTY) {
            new_depth = depth - 2;
        }

        if (i == 0) {
            score = -alpha_beta(g, new_depth, -beta, -alpha, nkey, 1);
        } else {
            /* PVS */
            score = -alpha_beta(g, new_depth, -alpha - 1, -alpha, nkey, 1);
            if (score > alpha && score < beta)
                score = -alpha_beta(g, new_depth, -beta, -alpha, nkey, 1);
        }

        unmake_move(g);

        if (score > best_score) {
            best_score = score;
            best_move = moves[i].from | (moves[i].to << 6);
        }
        if (score > alpha) {
            alpha = score;
            flag = 0; /* exact */
        }
        if (alpha >= beta) {
            flag = 1; /* lower bound */
            break;
        }
    }

    tt_store(key, depth, best_score, flag, best_move);
    return best_score;
}

/* Find best move for the AI */
static Move find_best_move(Game *g, int max_depth)
{
    Move moves[MAX_MOVES];
    int count, i, d;
    Move best = {0};
    uint64_t key = compute_hash(g);

    count = gen_legal(g, moves);
    if (count == 0) {
        best.from = -1;
        best.to = -1;
        return best;
    }
    if (count == 1) {
        return moves[0]; /* only one legal move */
    }

    /* Iterative deepening */
    for (d = 1; d <= max_depth; d++) {
        int score;
        int local_best = -1;
        int local_best_score = -INF - 1;

        /* TT move for root ordering */
        int tt_move = -1;
        tt_probe(key, d, -INF, INF, &score, &tt_move);
        order_moves(moves, count, tt_move);

        for (i = 0; i < count; i++) {
            make_move(g, &moves[i]);
            uint64_t nkey = key ^ zobrist_side;

            if (i == 0) {
                score = -alpha_beta(g, d - 1, -INF, -local_best_score, nkey, 1);
            } else {
                score = -alpha_beta(g, d - 1, -local_best_score - 1, -local_best_score, nkey, 1);
                if (score > local_best_score) {
                    score = -alpha_beta(g, d - 1, -INF, -local_best_score, nkey, 1);
                }
            }

            unmake_move(g);

            if (score > local_best_score) {
                local_best_score = score;
                local_best = i;
            }
        }

        if (local_best >= 0) {
            best = moves[local_best];
        }
    }

    return best;
}

/* ======================================================================
 * Terminal UI
 * ====================================================================== */

static const char *piece_str(int p)
{
    if (IS_EMPTY(p)) return " ";
    int pt = PIECE_TYPE(p);
    int white = IS_WHITE(p);
    switch (pt) {
    case PAWN:   return white ? "\xE2\x99\x99" : "\xE2\x99\x9F"; /* ♙ ♟ */
    case KNIGHT: return white ? "\xE2\x99\x98" : "\xE2\x99\x9E"; /* ♘ ♞ */
    case BISHOP: return white ? "\xE2\x99\x97" : "\xE2\x99\x9D"; /* ♗ ♝ */
    case ROOK:   return white ? "\xE2\x99\x96" : "\xE2\x99\x9C"; /* ♖ ♜ */
    case QUEEN:  return white ? "\xE2\x99\x95" : "\xE2\x99\x9B"; /* ♕ ♛ */
    case KING:   return white ? "\xE2\x99\x94" : "\xE2\x99\x9A"; /* ♔ ♚ */
    }
    return "?";
}

static void print_board(const Game *g, int last_from, int last_to, int flip)
{
    int r, c, sq;
    printf("\033[H\033[2J"); /* clear screen */
    printf("\r\n");
    printf("  ┌───┬───┬───┬───┬───┬───┬───┬───┐\r\n");

    for (r = 0; r < 8; r++) {
        int dr = flip ? (7 - r) : r;
        printf("%d │", 8 - dr);
        for (c = 0; c < 8; c++) {
            int dc = flip ? (7 - c) : c;
            sq = SQ(dr, dc);
            int p = g->board[sq];
            int is_light = (dr + dc) % 2 == 0;

            /* Check if this square is the last move */
            int is_last = (sq == last_from || sq == last_to);

            /* Check if king is in check */
            int is_king_check = 0;
            if (!IS_EMPTY(p) && PIECE_TYPE(p) == KING &&
                PIECE_COLOR(p) == g->side && in_check(g, g->side)) {
                is_king_check = 1;
            }

            /* Background color */
            if (is_king_check)
                printf("\033[41m"); /* red for check */
            else if (is_last)
                printf("\033[43m"); /* yellow for last move */
            else if (is_light)
                printf("\033[47m"); /* light background */
            else
                printf("\033[44m"); /* dark background */

            /* Foreground color */
            if (!IS_EMPTY(p) && IS_WHITE(p))
                printf("\033[97m"); /* bright white */
            else if (!IS_EMPTY(p))
                printf("\033[30m"); /* black */

            printf(" %s ", piece_str(p));
            printf("\033[0m");
            if (c < 7) printf("│");
        }
        printf("│\r\n");
        printf("\033[0m");
        if (r < 7) {
            printf("  ├───┼───┼───┼───┼───┼───┼───┼───┤\r\n");
        }
    }

    printf("  └───┴───┴───┴───┴───┴───┴───┴───┘\r\n");
    printf("    ");
    for (c = 0; c < 8; c++) {
        int dc = flip ? (7 - c) : c;
        printf("%c   ", 'a' + dc);
    }
    printf("\r\n");

    /* Side to move */
    printf("\r\n  %s to move\r\n", g->side == WHITE ? "White" : "Black");

    /* Castling rights */
    printf("  Castling: ");
    if (g->castle & 1) printf("K");
    if (g->castle & 2) printf("Q");
    if (g->castle & 4) printf("k");
    if (g->castle & 8) printf("q");
    if (g->castle == 0) printf("-");
    printf("\r\n");

    /* Check status */
    if (in_check(g, g->side))
        printf("  \033[31mCHECK!\033[0m\r\n");
}

static void show_help(void)
{
    printf("\r\n  \033[1mCommands:\033[0m\r\n");
    printf("  e2e4        - move piece from e2 to e4\r\n");
    printf("  e7e8q       - promotion (q=rOk, r=rook, b=bishop, n=knight)\r\n");
    printf("  \033[33mmoves\033[0m      - show all legal moves\r\n");
    printf("  \033[33mundo\033[0m       - undo last move pair\r\n");
    printf("  \033[33mnew\033[0m        - start new game\r\n");
    printf("  \033[33mflip\033[0m       - flip board orientation\r\n");
    printf("  \033[33mdifficulty\033[0m - change AI difficulty level\r\n");
    printf("  \033[33meval\033[0m       - show evaluation breakdown\r\n");
    printf("  \033[33mcaptures\033[0m   - show captured pieces\r\n");
    printf("  \033[33mhistory\033[0m    - show move history\r\n");
    printf("  \033[33mbalance\033[0m    - show material balance\r\n");
    printf("  \033[33mfen\033[0m        - show FEN string\r\n");
    printf("  \033[33mquit\033[0m        - exit\r\n");
    printf("  \033[33mhelp\033[0m        - this message\r\n");
    printf("  Mouse: click piece, then click destination\r\n\r\n");
}

/* ======================================================================
 * Move parsing
 * ====================================================================== */

static int parse_move(const char *input, Game *g, Move *out)
{
    Move legal[MAX_MOVES];
    int count = gen_legal(g, legal);
    int i;
    int from_col = -1, from_row = -1, to_col = -1, to_row = -1;
    int promo = EMPTY;
    const char *p = input;

    /* Skip whitespace */
    while (*p == ' ') p++;

    /* Parse from square */
    if (p[0] >= 'a' && p[0] <= 'h') from_col = p[0] - 'a';
    else if (p[0] >= 'A' && p[0] <= 'H') from_col = p[0] - 'A';
    else return -1;
    p++;

    if (p[0] >= '1' && p[0] <= '8') from_row = 8 - (p[0] - '0');
    else return -1;
    p++;

    /* Optional separator */
    if (*p == '-' || *p == 'x') p++;

    /* Parse to square */
    if (p[0] >= 'a' && p[0] <= 'h') to_col = p[0] - 'a';
    else if (p[0] >= 'A' && p[0] <= 'H') to_col = p[0] - 'A';
    else return -1;
    p++;

    if (p[0] >= '1' && p[0] <= '8') to_row = 8 - (p[0] - '0');
    else return -1;
    p++;

    /* Optional promotion piece */
    if (*p == 'q' || *p == 'Q') promo = QUEEN;
    else if (*p == 'r' || *p == 'R') promo = ROOK;
    else if (*p == 'b' || *p == 'B') promo = BISHOP;
    else if (*p == 'n' || *p == 'N') promo = KNIGHT;

    int from = SQ(from_row, from_col);
    int to = SQ(to_row, to_col);

    /* Find matching legal move */
    for (i = 0; i < count; i++) {
        if (legal[i].from == from && legal[i].to == to) {
            if (promo != EMPTY) {
                if (legal[i].promoted == promo) {
                    *out = legal[i];
                    return 0;
                }
            } else {
                if (legal[i].promoted == EMPTY) {
                    *out = legal[i];
                    return 0;
                }
                /* If only one promotion choice, use it */
            }
        }
    }

    /* If promotion specified but not found with that piece, try any promotion */
    if (promo != EMPTY) {
        for (i = 0; i < count; i++) {
            if (legal[i].from == from && legal[i].to == to && legal[i].promoted != EMPTY) {
                *out = legal[i];
                return 0;
            }
        }
    }

    return -1;
}

static int parse_move(const char *input, Game *g, Move *out);

/* ======================================================================
 * Mouse protocol (SGR xterm)
 * ====================================================================== */

#ifdef HAS_TERMIOS
static struct termios g_orig_termios;
static int g_termios_saved = 0;

static void mouse_enable(void)
{
    if (!g_termios_saved) {
        tcgetattr(STDIN_FILENO, &g_orig_termios);
        g_termios_saved = 1;
    }
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    printf("\033[?1000h\033[?1006h");
    fflush(stdout);
}

static void mouse_disable(void)
{
    printf("\033[?1000l\033[?1006l");
    fflush(stdout);
    if (g_termios_saved)
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
}
#else
static void mouse_enable(void)
{
    printf("\033[?1000h\033[?1006h");
    fflush(stdout);
}

static void mouse_disable(void)
{
    printf("\033[?1000l\033[?1006l");
    fflush(stdout);
}
#endif

/* Map terminal column to board column (0-7), or -1 if outside board.
 * Board layout (flip=0, after \033[H\033[2J clears screen):
 *   col a center = 0-based terminal col 4, each cell = 4 chars wide
 *   col b center = col 8, col c = 12, ... col h = 32
 *   (SGR mouse coords are 1-based, caller passes col-1)
 */
static int term_col_to_board(int term_col, int flip)
{
    int base_col = 4;
    int board_col = (term_col - base_col) / 4;
    if (board_col < 0 || board_col > 7) return -1;
    if (flip) board_col = 7 - board_col;
    return board_col;
}

/* Map terminal row to board row (0=rank8 .. 7=rank1), or -1 if outside.
 * Board layout (flip=0, after \033[H\033[2J clears screen):
 *   rank 8 at 0-based terminal row 2, rank 7 at row 4, ... rank 1 at row 16
 *   Each board row = 2 terminal lines (piece + separator)
 *   (SGR mouse coords are 1-based, caller passes row-1)
 */
static int term_row_to_board(int term_row, int flip)
{
    int base_row = 2;
    int board_row = (term_row - base_row) / 2;
    if (board_row < 0 || board_row > 7) return -1;
    if (flip) board_row = 7 - board_row;
    return board_row;
}

/* Read a line from stdin, handling SGR mouse escape sequences.
 * Returns:
 *   0 = normal text in `buf` (e.g. "e2e4", "help")
 *   1 = mouse click at board square (mouse_sq set: row*8+col)
 *   2 = mouse release (ignored)
 *  -1 = EOF
 */
static int read_input_with_mouse(char *buf, int bufsize, int *mouse_sq, int flip)
{
    int i = 0;
    int state = 0;
    int btn = 0, col = 0, row = 0;
    int field = 0; /* 0=btn, 1=col, 2=row */
    int negate = 0;
    int *target = &btn;

    buf[0] = '\0';

    while (1) {
        int ch = fgetc(stdin);
        if (ch < 0) return -1;

        if (state == 0) {
            if (ch == 27) { state = 1; }
            else if (ch == '\n' || ch == '\r') { buf[i] = '\0'; return 0; }
            else if (i < bufsize - 1) { buf[i++] = (char)ch; }
        } else if (state == 1) {
            state = (ch == '[') ? 2 : 0;
            if (state == 0 && i < bufsize - 1) { buf[i++] = 27; buf[i++] = (char)ch; }
        } else if (state == 2) {
            if (ch == '<') {
                state = 3; btn = 0; col = 0; row = 0; field = 0; negate = 0;
                target = &btn;
            } else {
                state = 0;
                if (i < bufsize - 1) { buf[i++] = 27; buf[i++] = '['; buf[i++] = (char)ch; }
            }
        } else if (state == 3) {
            if (ch >= '0' && ch <= '9') {
                *target = *target * 10 + (negate ? -(ch - '0') : (ch - '0'));
            } else if (ch == ';') {
                field++;
                if (field == 1) { target = &col; col = 0; negate = 0; }
                else if (field == 2) { target = &row; row = 0; negate = 0; }
            } else if (ch == 'M' || ch == 'm') {
                state = 0;
                if (ch == 'M' && (btn & 0x03) == 0) {
                    int bc = term_col_to_board(col - 1, flip);
                    int br = term_row_to_board(row - 1, flip);
                    if (bc >= 0 && br >= 0) { *mouse_sq = br * 8 + bc; return 1; }
                }
                return 2;
            } else if (ch == '-') {
                negate = 1;
            } else {
                state = 0;
            }
        }
    }
}

static void show_legal_moves(const Game *g)
{
    Move moves[MAX_MOVES];
    int count = gen_legal(g, moves);
    int i;
    char cols[] = "abcdefgh";

    printf("\r\n  Legal moves (%d):\r\n  ", count);
    for (i = 0; i < count; i++) {
        int fc = COL(moves[i].from);
        int fr = 8 - ROW(moves[i].from);
        int tc = COL(moves[i].to);
        int tr = 8 - ROW(moves[i].to);
        printf("%c%d%c%d", cols[fc], fr, cols[tc], tr);
        if (moves[i].promoted != EMPTY) {
            switch (moves[i].promoted) {
            case QUEEN:  printf("q"); break;
            case ROOK:   printf("r"); break;
            case BISHOP: printf("b"); break;
            case KNIGHT: printf("n"); break;
            }
        }
        printf(" ");
    }
    printf("\r\n\r\n");
}

static const char *difficulty_name(int d)
{
    switch (d) {
    case DIFF_EASY:   return "Easy";
    case DIFF_MEDIUM: return "Medium";
    case DIFF_HARD:   return "Hard";
    case DIFF_EXPERT: return "Expert";
    default:          return "Unknown";
    }
}

static int difficulty_depth(int d)
{
    switch (d) {
    case DIFF_EASY:   return 2;
    case DIFF_MEDIUM: return 3;
    case DIFF_HARD:   return 5;
    case DIFF_EXPERT: return 6;
    default:          return 4;
    }
}

static void print_difficulty_menu(int current)
{
    int i;
    printf("\r\n  \033[1mSelect difficulty:\033[0m\r\n");
    for (i = 0; i < NUM_DIFFICULTIES; i++) {
        const char *tag = (i == DIFF_EASY) ? "(beginner)" :
                          (i == DIFF_MEDIUM) ? "(casual)" :
                          (i == DIFF_HARD) ? "(strong)" : "(expert)";
        if (i == current)
            printf("    \033[32m%d) %-8s %s\033[0m\r\n", i + 1, difficulty_name(i), tag);
        else
            printf("    %d) %-8s %s\r\n", i + 1, difficulty_name(i), tag);
    }
    printf("\r\n  Current: \033[33m%s\033[0m (depth %d)\r\n", difficulty_name(current), difficulty_depth(current));
    printf("  Your choice [1-%d]: ", NUM_DIFFICULTIES);
}

static int material_balance(const Game *g)
{
    int sq, balance = 0;
    for (sq = 0; sq < 64; sq++) {
        int p = g->board[sq];
        if (IS_EMPTY(p)) continue;
        int val = 0;
        switch (PIECE_TYPE(p)) {
        case PAWN:   val = VAL_PAWN; break;
        case KNIGHT: val = VAL_KNIGHT; break;
        case BISHOP: val = VAL_BISHOP; break;
        case ROOK:   val = VAL_ROOK; break;
        case QUEEN:  val = VAL_QUEEN; break;
        default: continue;
        }
        balance += IS_WHITE(p) ? val : -val;
    }
    return balance;
}

static void print_captures(const Game *g)
{
    int i;
    static const char piece_chars[] = ".PNBRQK";
    printf("\r\n  \033[1mCaptured pieces:\033[0m\r\n");

    printf("  White lost: ");
    {
        int total = 0;
        for (i = 1; i <= 5; i++) total += g->white_captured[i];
        if (total == 0) { printf("none"); }
        else {
            for (i = 5; i >= 1; i--) {
                int j;
                for (j = 0; j < g->white_captured[i]; j++)
                    printf("%c ", piece_chars[i]);
            }
        }
    }
    printf("\r\n");

    printf("  Black lost: ");
    {
        int total = 0;
        for (i = 1; i <= 5; i++) total += g->black_captured[i];
        if (total == 0) { printf("none"); }
        else {
            for (i = 5; i >= 1; i--) {
                int j;
                for (j = 0; j < g->black_captured[i]; j++)
                    printf("%c ", piece_chars[i]);
            }
        }
    }
    printf("\r\n\r\n");
}

static void print_history(const Game *g)
{
    int i, start;
    printf("\r\n  \033[1mMove history:\033[0m\r\n  ");
    start = (g->notation_count > 20) ? g->notation_count - 20 : 0;
    for (i = start; i < g->notation_count; i++) {
        if (i % 2 == 0)
            printf("%d. ", i / 2 + 1);
        printf("%s ", g->move_notation[i]);
        if (i % 2 == 1 || i == g->notation_count - 1) {
            printf("\r\n  ");
        }
    }
    printf("\r\n");
}

static void print_eval_breakdown(void)
{
    int total = eval_material + eval_position + eval_king_safety +
                eval_pawn_structure + eval_mobility + eval_center + eval_threats;
    printf("\r\n  \033[1mEvaluation breakdown (from White's perspective):\033[0m\r\n");
    printf("  Material:       %+6d\r\n", eval_material);
    printf("  Position (PST): %+6d\r\n", eval_position);
    printf("  King Safety:    %+6d\r\n", eval_king_safety);
    printf("  Pawn Structure: %+6d\r\n", eval_pawn_structure);
    printf("  Mobility:       %+6d\r\n", eval_mobility);
    printf("  Center:         %+6d\r\n", eval_center);
    printf("  Threats:        %+6d\r\n", eval_threats);
    printf("  ─────────────────────\r\n");
    printf("  Total:          %+6d  (\033[33m%.2f pawns\033[0m)\r\n\r\n",
           total, total / 100.0);
}

static void print_fen(const Game *g)
{
    int sq, empty = 0;
    printf("\r\n  FEN: ");
    for (sq = 0; sq < 64; sq++) {
        int r = ROW(sq), c = COL(sq);
        int p = g->board[sq];
        (void)r;
        if (c == 0 && sq > 0) {
            if (empty > 0) { printf("%d", empty); empty = 0; }
            printf("/");
        }
        if (IS_EMPTY(p)) { empty++; continue; }
        if (empty > 0) { printf("%d", empty); empty = 0; }
        int pt = PIECE_TYPE(p);
        char ch = (pt == PAWN) ? 'p' : (pt == KNIGHT) ? 'n' : (pt == BISHOP) ? 'b' :
                  (pt == ROOK) ? 'r' : (pt == QUEEN) ? 'q' : (pt == KING) ? 'k' : '?';
        if (IS_WHITE(p)) ch = ch - 'a' + 'A';
        printf("%c", ch);
    }
    if (empty > 0) printf("%d", empty);

    printf(" %s", g->side == WHITE ? "w" : "b");
    printf(" ");
    if (g->castle == 0) printf("-");
    else {
        if (g->castle & 1) printf("K");
        if (g->castle & 2) printf("Q");
        if (g->castle & 4) printf("k");
        if (g->castle & 8) printf("q");
    }
    printf(" ");
    if (g->ep_square < 0) printf("-");
    else printf("%c%d", 'a' + COL(g->ep_square), 8 - ROW(g->ep_square));
    printf(" %d %d\r\n\r\n", g->halfmove_clock, g->fullmove);
}

static void print_material_balance(const Game *g)
{
    int balance = material_balance(g);
    printf("\r\n  Material balance: ");
    if (balance > 0) printf("White +%d", balance);
    else if (balance < 0) printf("Black +%d", -balance);
    else printf("Equal");
    printf(" (\033[33m%+.2f pawns\033[0m)\r\n\r\n", balance / 100.0);
}

static void record_move(Game *g, const Move *m)
{
    int pt = PIECE_TYPE(m->piece);
    char *dest = g->move_notation[g->notation_count];
    int idx = 0;
    char cols[] = "abcdefgh";

    if (m->castle == 1) { dest[idx++] = 'O'; dest[idx++] = '-'; dest[idx++] = 'O'; }
    else if (m->castle == 2) {
        dest[idx++] = 'O'; dest[idx++] = '-'; dest[idx++] = 'O';
        dest[idx++] = '-'; dest[idx++] = 'O';
    }
    else {
        if (pt != PAWN) {
            dest[idx++] = (pt == KNIGHT) ? 'N' : (pt == BISHOP) ? 'B' :
                          (pt == ROOK) ? 'R' : (pt == QUEEN) ? 'Q' : 'K';
        }
        if (!IS_EMPTY(m->captured) || m->en_passant) {
            if (pt == PAWN) dest[idx++] = cols[COL(m->from)];
            dest[idx++] = 'x';
        }
        dest[idx++] = cols[COL(m->to)];
        dest[idx++] = '0' + (8 - ROW(m->to));
        if (m->promoted != EMPTY) {
            dest[idx++] = '=';
            dest[idx++] = (m->promoted == QUEEN) ? 'Q' : (m->promoted == ROOK) ? 'R' :
                          (m->promoted == BISHOP) ? 'B' : 'N';
        }
    }
    dest[idx] = '\0';
    g->notation_count++;
}

/* ======================================================================
 * Main game loop
 * ====================================================================== */

int main(void)
{
    Game game;
    int flip = 0;
    int last_from = -1, last_to = -1;
    char input[256];
    int ai_depth = DEFAULT_DEPTH;
    int selected_sq = -1;

    init_zobrist();
    tt_init();

    printf("\r\n");
    printf("  ╔══════════════════════════════════════╗\r\n");
    printf("  ║     A20OS Chess — Native ABI        ║\r\n");
    printf("  ║     AI Engine                       ║\r\n");
    printf("  ╚══════════════════════════════════════╝\r\n");

    print_difficulty_menu(g_difficulty);
    fflush(stdout);
    {
        int choice = 0;
        if (fgets(input, sizeof(input), stdin)) {
            choice = input[0] - '0';
            if (choice >= 1 && choice <= NUM_DIFFICULTIES)
                g_difficulty = choice - 1;
            else
                g_difficulty = DIFF_HARD;
        }
    }
    ai_depth = difficulty_depth(g_difficulty);

    init_game:

    init_board(&game);
    last_from = -1;
    last_to = -1;

    printf("\r\n");
    printf("  ╔══════════════════════════════════════╗\r\n");
    printf("  ║     A20OS Chess — Native ABI        ║\r\n");
    printf("  ║     Difficulty: %-8s (depth %d)  ║\r\n", difficulty_name(g_difficulty), ai_depth);
    printf("  ╚══════════════════════════════════════╝\r\n");
    printf("\r\n");
    printf("  Type '\033[33mhelp\033[0m' for commands.\r\n");
    printf("  You play \033[31mBlack\033[0m. AI plays \033[37mWhite\033[0m.\r\n");
    printf("  Mouse: click piece, then click destination.\r\n\r\n");

    atexit(mouse_disable);
    mouse_enable();

    while (1) {
        int game_over = 0;
        Move legal[MAX_MOVES];
        int legal_count = gen_legal(&game, legal);

        if (legal_count == 0) {
            print_board(&game, last_from, last_to, flip);
            if (in_check(&game, game.side)) {
                printf("\r\n  \033[31;1mCHECKMATE! %s wins!\033[0m\r\n\r\n",
                       game.side == WHITE ? "Black" : "White");
            } else {
                printf("\r\n  \033[33;1mSTALEMATE — Draw!\033[0m\r\n\r\n");
            }
            game_over = 1;
        }

        if (game.halfmove_clock >= 100) {
            print_board(&game, last_from, last_to, flip);
            printf("\r\n  \033[33;1m50-MOVE RULE — Draw!\033[0m\r\n\r\n");
            game_over = 1;
        }

        if (game_over) {
            print_material_balance(&game);
            printf("  Commands: '\033[33mnew\033[0m' for new game, '\033[33mquit\033[0m' to exit\r\n");
            printf("  > ");
            fflush(stdout);
            if (!fgets(input, sizeof(input), stdin)) break;
            {
                int len = strlen(input);
                while (len > 0 && (input[len-1] == '\n' || input[len-1] == '\r'))
                    input[--len] = '\0';
            }
            if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) break;
            if (strcmp(input, "new") == 0) { selected_sq = -1; goto init_game; }
            continue;
        }

        if (game.side == WHITE) {
            print_board(&game, last_from, last_to, flip);
            {
                int bal = material_balance(&game);
                printf("\r\n  \033[37mAI\033[0m is thinking... (\033[33m%s\033[0m, depth %d)",
                       difficulty_name(g_difficulty), ai_depth);
                if (bal != 0) printf(" | Material: %+.0f", bal / 100.0);
                printf("\r\n");
            }
            fflush(stdout);

            Move best = find_best_move(&game, ai_depth);
            if (best.from < 0) {
                printf("  No legal moves!\r\n");
                break;
            }

            int pt = PIECE_TYPE(best.piece);
            const char *pname = "";
            switch (pt) {
            case PAWN: pname = "Pawn"; break;
            case KNIGHT: pname = "Knight"; break;
            case BISHOP: pname = "Bishop"; break;
            case ROOK: pname = "Rook"; break;
            case QUEEN: pname = "Queen"; break;
            case KING: pname = "King"; break;
            }
            record_move(&game, &best);
            printf("  \033[37mAI plays:\033[0m %c%d%c%d (%s)%s%s [%s]\r\n",
                   'a' + COL(best.from), 8 - ROW(best.from),
                   'a' + COL(best.to), 8 - ROW(best.to),
                   pname,
                   best.promoted != EMPTY ? " promote to " : "",
                   best.promoted == QUEEN ? "Queen" :
                   best.promoted == ROOK ? "Rook" :
                   best.promoted == BISHOP ? "Bishop" :
                   best.promoted == KNIGHT ? "Knight" : "",
                   game.move_notation[game.notation_count - 1]);

            last_from = best.from;
            last_to = best.to;
            make_move(&game, &best);
            continue;
        }

        print_board(&game, last_from, last_to, flip);
        {
            int bal = material_balance(&game);
            if (bal != 0) printf("  Material: %+.0f", bal / 100.0);
            if (in_check(&game, game.side))
                printf("  \033[31;1mCHECK!\033[0m");
            printf("\r\n");
        }
        if (selected_sq >= 0) {
            int sc = COL(selected_sq);
            int sr = 8 - ROW(selected_sq);
            printf("  \033[33mSelected:\033[0m %c%d — click destination or type move\r\n", 'a' + sc, sr);
        } else {
            printf("  \033[32mYour move:\033[0m ");
        }
        fflush(stdout);

        int mouse_sq = -1;
        int result = read_input_with_mouse(input, sizeof(input), &mouse_sq, flip);

        if (result == -1) break;

        if (result == 1 && mouse_sq >= 0) {
            int mc = COL(mouse_sq);
            int mr = 8 - ROW(mouse_sq);
            int piece = game.board[mouse_sq];

            if (selected_sq < 0) {
                if (!IS_EMPTY(piece) && PIECE_COLOR(piece) == game.side) {
                    selected_sq = mouse_sq;
                    printf("  Selected %c%d\r\n", 'a' + mc, mr);
                }
            } else {
                if (mouse_sq == selected_sq) {
                    selected_sq = -1;
                } else {
                    char move_str[8];
                    move_str[0] = 'a' + COL(selected_sq);
                    move_str[1] = '0' + (8 - ROW(selected_sq));
                    move_str[2] = 'a' + mc;
                    move_str[3] = '0' + mr;
                    move_str[4] = '\0';
                    Move m;
                    if (parse_move(move_str, &game, &m) == 0) {
                        record_move(&game, &m);
                        last_from = m.from;
                        last_to = m.to;
                        make_move(&game, &m);
                        selected_sq = -1;
                    } else {
                        if (!IS_EMPTY(piece) && PIECE_COLOR(piece) == game.side) {
                            selected_sq = mouse_sq;
                            printf("  Selected %c%d\r\n", 'a' + mc, mr);
                        } else {
                            printf("  \033[31mInvalid move\033[0m from %c%d to %c%d\r\n",
                                   'a' + COL(selected_sq), 8 - ROW(selected_sq),
                                   'a' + mc, mr);
                        }
                    }
                }
            }
            continue;
        }

        {
            int len = strlen(input);
            while (len > 0 && (input[len-1] == '\n' || input[len-1] == '\r'))
                input[--len] = '\0';
        }

        if (strlen(input) == 0) continue;

        if (strcmp(input, "quit") == 0 || strcmp(input, "exit") == 0) break;
        if (strcmp(input, "new") == 0) { selected_sq = -1; goto init_game; }
        if (strcmp(input, "help") == 0) { show_help(); continue; }
        if (strcmp(input, "flip") == 0) { flip = !flip; selected_sq = -1; continue; }
        if (strcmp(input, "moves") == 0) { show_legal_moves(&game); continue; }
        if (strcmp(input, "difficulty") == 0 || strcmp(input, "level") == 0) {
            mouse_disable();
            print_difficulty_menu(g_difficulty);
            fflush(stdout);
            if (fgets(input, sizeof(input), stdin)) {
                int choice = input[0] - '0';
                if (choice >= 1 && choice <= NUM_DIFFICULTIES) {
                    g_difficulty = choice - 1;
                    ai_depth = difficulty_depth(g_difficulty);
                    printf("  Difficulty set to \033[33m%s\033[0m (depth %d)\r\n\r\n",
                           difficulty_name(g_difficulty), ai_depth);
                }
            }
            mouse_enable();
            selected_sq = -1;
            continue;
        }
        if (strcmp(input, "eval") == 0) { evaluate(&game); print_eval_breakdown(); continue; }
        if (strcmp(input, "captures") == 0) { print_captures(&game); continue; }
        if (strcmp(input, "history") == 0) { print_history(&game); continue; }
        if (strcmp(input, "balance") == 0) { print_material_balance(&game); continue; }
        if (strcmp(input, "fen") == 0) { print_fen(&game); continue; }
        if (strcmp(input, "undo") == 0) {
            if (game.history_len >= 2) {
                unmake_move(&game);
                unmake_move(&game);
                last_from = -1;
                last_to = -1;
                selected_sq = -1;
                printf("  Undone.\r\n");
            } else {
                printf("  Nothing to undo.\r\n");
            }
            continue;
        }

        selected_sq = -1;

        {
            Move m;
            if (parse_move(input, &game, &m) < 0) {
                printf("  \033[31mInvalid move.\033[0m Use format: e2e4 (type '\033[33mhelp\033[0m' for commands)\r\n");
                continue;
            }
            record_move(&game, &m);
            last_from = m.from;
            last_to = m.to;
            make_move(&game, &m);
        }
    }

    mouse_disable();
    printf("\r\n  Thanks for playing!\r\n\r\n");
    return 0;
}
