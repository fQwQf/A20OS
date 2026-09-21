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

/* Castling rights */
#define WK 1
#define WQ 2
#define BK 4
#define BQ 8

/* Transposition-table bound types */
#define TT_EXACT 0
#define TT_LOWER 1
#define TT_UPPER 2

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
/* Maximum number of half-moves kept on the game/search stack.  A full game can
 * easily exceed 64 plies, so this must comfortably cover a whole game plus the
 * deepest search recursion; the value below is also used for the notation
 * ring.  copy_position() avoids duplicating this array on every search node,
 * so growing it does not cost per-node memcpy time. */
#define MAX_UNDO   1024
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
    int castle;      /* 0=none, 1=kingside, 2=queenside */
    int en_passant;  /* 1 if en passant capture */
    int prev_ep;     /* undo state, filled in by make_move */
    int prev_castle;
    int halfmove;
    uint64_t prev_hash;
} Move;

typedef struct {
    int board[64];
    int side;          /* WHITE or BLACK */
    int ep_square;     /* en passant target square, -1 if none */
    int castle;        /* castling rights bitmask, see WK/WQ/BK/BQ */
    int halfmove_clock;
    int fullmove;
    uint64_t hash;     /* Zobrist key, kept in sync by make_move/unmake_move */
    Move history[MAX_UNDO];
    int history_len;
    int white_captured[7];
    int black_captured[7];
    char move_notation[MAX_UNDO][8];
    int notation_count;
} Game;

/* Transposition table */
typedef struct {
    uint64_t key;
    int depth;
    int score;
    int flag;   /* TT_EXACT / TT_LOWER / TT_UPPER */
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
static uint64_t zobrist_ep[8];   /* indexed by ep file */
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
    for (p = 0; p < 8; p++)
        zobrist_ep[p] = ((uint64_t)xorshift32() << 32) | xorshift32();
    zobrist_side = ((uint64_t)xorshift32() << 32) | xorshift32();
    rng_state = 0xDEADBEEF;
    zobrist_initialized = 1;
}

static uint64_t piece_key(int sq, int piece)
{
    return zobrist_pieces[sq][PIECE_TYPE(piece)][IS_WHITE(piece) ? 0 : 1];
}

static uint64_t compute_hash(const Game *g)
{
    uint64_t h = 0;
    int sq;
    for (sq = 0; sq < 64; sq++) {
        int p = g->board[sq];
        if (!IS_EMPTY(p))
            h ^= piece_key(sq, p);
    }
    h ^= zobrist_castle[g->castle & 0xF];
    if (g->ep_square >= 0)
        h ^= zobrist_ep[COL(g->ep_square)];
    if (g->side == BLACK)
        h ^= zobrist_side;
    return h;
}

/* TT move is packed as from | (to << 6); both fit in six bits. */
static int move_key(int from, int to) { return from | (to << 6); }
static int move_key_from(int key) { return key & 0x3F; }
static int move_key_to(int key) { return (key >> 6) & 0x3F; }

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
        if (e->flag == TT_EXACT) {
            *score = e->score;
            return 1;
        }
        if (e->flag == TT_LOWER && e->score >= beta) {
            *score = e->score;
            return 1;
        }
        if (e->flag == TT_UPPER && e->score <= alpha) {
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
    g->castle = WK | WQ | BK | BQ;
    g->halfmove_clock = 0;
    g->fullmove = 1;
    g->history_len = 0;
    g->notation_count = 0;
    memset(g->white_captured, 0, sizeof(g->white_captured));
    memset(g->black_captured, 0, sizeof(g->black_captured));
    g->hash = compute_hash(g);
}

/* Copy the board and game state but not the history stacks.  The search copies
 * a position at every node, so duplicating MAX_UNDO entries would dominate
 * runtime; searches always start from an empty stack they unwind themselves. */
static void copy_position(Game *dst, const Game *src)
{
    memcpy(dst->board, src->board, sizeof(dst->board));
    dst->side = src->side;
    dst->ep_square = src->ep_square;
    dst->castle = src->castle;
    dst->halfmove_clock = src->halfmove_clock;
    dst->fullmove = src->fullmove;
    dst->hash = src->hash;
    dst->history_len = 0;
    dst->notation_count = 0;
    memset(dst->white_captured, 0, sizeof(dst->white_captured));
    memset(dst->black_captured, 0, sizeof(dst->black_captured));
}

/* ======================================================================
 * Move generation
 * ====================================================================== */

static int in_bounds(int r, int c)
{
    return r >= 0 && r < 8 && c >= 0 && c < 8;
}

/* Is `sq` attacked by any piece of `by_side`? */
static int is_attacked(const Game *g, int sq, int by_side)
{
    static const int knight[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    static const int king[8][2]   = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
    static const int diag[4][2]   = {{-1,-1},{-1,1},{1,-1},{1,1}};
    static const int orth[4][2]   = {{-1,0},{1,0},{0,-1},{0,1}};
    int r = ROW(sq), c = COL(sq);
    int i;

    for (i = 0; i < 8; i++)
        if (in_bounds(r + knight[i][0], c + knight[i][1]) &&
            g->board[SQ(r + knight[i][0], c + knight[i][1])] == (KNIGHT | by_side))
            return 1;

    if (by_side == WHITE) {
        if (r < 7 && c > 0 && g->board[SQ(r+1, c-1)] == (PAWN | WHITE)) return 1;
        if (r < 7 && c < 7 && g->board[SQ(r+1, c+1)] == (PAWN | WHITE)) return 1;
    } else {
        if (r > 0 && c > 0 && g->board[SQ(r-1, c-1)] == (PAWN | BLACK)) return 1;
        if (r > 0 && c < 7 && g->board[SQ(r-1, c+1)] == (PAWN | BLACK)) return 1;
    }

    for (i = 0; i < 4; i++) {
        int nr = r + diag[i][0], nc = c + diag[i][1];
        while (in_bounds(nr, nc)) {
            int p = g->board[SQ(nr, nc)];
            if (!IS_EMPTY(p)) {
                if (p == (BISHOP | by_side) || p == (QUEEN | by_side)) return 1;
                break;
            }
            nr += diag[i][0]; nc += diag[i][1];
        }
        nr = r + orth[i][0]; nc = c + orth[i][1];
        while (in_bounds(nr, nc)) {
            int p = g->board[SQ(nr, nc)];
            if (!IS_EMPTY(p)) {
                if (p == (ROOK | by_side) || p == (QUEEN | by_side)) return 1;
                break;
            }
            nr += orth[i][0]; nc += orth[i][1];
        }
    }

    for (i = 0; i < 8; i++)
        if (in_bounds(r + king[i][0], c + king[i][1]) &&
            g->board[SQ(r + king[i][0], c + king[i][1])] == (KING | by_side))
            return 1;

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

static void add_move(Move *moves, int *count, int from, int to, int piece,
                     int captured, int promoted, int castle, int en_passant)
{
    if (*count >= MAX_MOVES)
        return;
    Move *m = &moves[(*count)++];
    m->from = from;
    m->to = to;
    m->piece = piece;
    m->captured = captured;
    m->promoted = promoted;
    m->castle = castle;
    m->en_passant = en_passant;
}

/* Castling needs intact rights, a rook at home, a clear path, and a king that
 * neither starts on, crosses, nor lands on an attacked square. */
static int castle_legal(const Game *g, int side, int kingside)
{
    int row = side == WHITE ? 7 : 0;
    int step = kingside ? 1 : -1;
    int enemy = side == WHITE ? BLACK : WHITE;
    int right = kingside ? (side == WHITE ? WK : BK) : (side == WHITE ? WQ : BQ);

    if (!(g->castle & right) || g->board[SQ(row, kingside ? 7 : 0)] != (ROOK | side))
        return 0;
    if (!IS_EMPTY(g->board[SQ(row, 4 + step)]) ||
        !IS_EMPTY(g->board[SQ(row, 4 + 2 * step)]))
        return 0;
    if (!kingside && !IS_EMPTY(g->board[SQ(row, 1)]))
        return 0;
    return !is_attacked(g, SQ(row, 4), enemy) &&
           !is_attacked(g, SQ(row, 4 + step), enemy) &&
           !is_attacked(g, SQ(row, 4 + 2 * step), enemy);
}

/* Could a piece standing on `from` move to `to`?  Ignores occupancy of `from`
 * (callers pass a square holding that piece type), and is used for SAN
 * disambiguation only, so it just needs the movement geometry. */
static int piece_reaches(const Game *g, int from, int to)
{
    int type = PIECE_TYPE(g->board[from]);
    int r = ROW(from), c = COL(from);
    int tr = ROW(to), tc = COL(to);

    if (type == KNIGHT) {
        int dr = abs_val(tr - r), dc = abs_val(tc - c);
        return (dr == 1 && dc == 2) || (dr == 2 && dc == 1);
    }
    if (type == BISHOP)
        return abs_val(tr - r) == abs_val(tc - c) && tr != r;
    if (type == ROOK)
        return (tr == r) != (tc == c);
    if (type == QUEEN)
        return (tr == r || tc == c) ||
               (abs_val(tr - r) == abs_val(tc - c) && tr != r);
    return 0;
}

/* All pseudo-legal moves, ignoring whether the king is left in check. */
static int gen_pseudo_legal(const Game *g, Move *moves)
{
    static const int knight[8][2] = {{-2,-1},{-2,1},{-1,-2},{-1,2},{1,-2},{1,2},{2,-1},{2,1}};
    static const int king[8][2]   = {{-1,-1},{-1,0},{-1,1},{0,-1},{0,1},{1,-1},{1,0},{1,1}};
    static const int slide[8][2]  = {{-1,-1},{-1,1},{1,-1},{1,1},{-1,0},{1,0},{0,-1},{0,1}};
    int side = g->side;
    int enemy = side == WHITE ? BLACK : WHITE;
    int count = 0;

    for (int sq = 0; sq < 64; sq++) {
        int p = g->board[sq];
        if (IS_EMPTY(p) || PIECE_COLOR(p) != side)
            continue;
        int r = ROW(sq), c = COL(sq);
        int type = PIECE_TYPE(p);

        if (type == PAWN) {
            int dir = side == WHITE ? -1 : 1;
            int last = side == WHITE ? 0 : 7;
            int start = side == WHITE ? 6 : 1;
            int fwd = SQ(r + dir, c);
            if (IS_EMPTY(g->board[fwd])) {
                if (r + dir == last) {
                    for (int q = QUEEN; q >= KNIGHT; q--)
                        add_move(moves, &count, sq, fwd, p, EMPTY, q, 0, 0);
                } else {
                    add_move(moves, &count, sq, fwd, p, EMPTY, EMPTY, 0, 0);
                    if (r == start && IS_EMPTY(g->board[SQ(r + 2 * dir, c)]))
                        add_move(moves, &count, sq, SQ(r + 2 * dir, c), p, EMPTY, EMPTY, 0, 0);
                }
            }
            for (int dc = -1; dc <= 1; dc += 2) {
                int nr = r + dir, nc = c + dc;
                if (!in_bounds(nr, nc))
                    continue;
                int target = g->board[SQ(nr, nc)];
                int ep = (g->ep_square == SQ(nr, nc));
                if (IS_EMPTY(target) ? !ep : PIECE_COLOR(target) == side)
                    continue;
                int captured = ep ? (PAWN | enemy) : target;
                if (nr == last) {
                    for (int q = QUEEN; q >= KNIGHT; q--)
                        add_move(moves, &count, sq, SQ(nr, nc), p, captured, q, 0, ep);
                } else {
                    add_move(moves, &count, sq, SQ(nr, nc), p, captured, EMPTY, 0, ep);
                }
            }
        } else if (type == KNIGHT || type == KING) {
            const int (*steps)[2] = type == KNIGHT ? knight : king;
            for (int i = 0; i < 8; i++) {
                int nr = r + steps[i][0], nc = c + steps[i][1];
                if (!in_bounds(nr, nc))
                    continue;
                int target = g->board[SQ(nr, nc)];
                if (!IS_EMPTY(target) && PIECE_COLOR(target) == side)
                    continue;
                add_move(moves, &count, sq, SQ(nr, nc), p, target, EMPTY, 0, 0);
            }
            if (type == KING && c == 4 && r == (side == WHITE ? 7 : 0)) {
                if (castle_legal(g, side, 1))
                    add_move(moves, &count, sq, SQ(r, 6), p, EMPTY, EMPTY, 1, 0);
                if (castle_legal(g, side, 0))
                    add_move(moves, &count, sq, SQ(r, 2), p, EMPTY, EMPTY, 2, 0);
            }
        } else {
            int first = type == BISHOP ? 0 : type == ROOK ? 4 : 0;
            int stop = type == BISHOP ? 4 : type == ROOK ? 8 : 8;
            for (int i = first; i < stop; i++) {
                int nr = r + slide[i][0], nc = c + slide[i][1];
                while (in_bounds(nr, nc)) {
                    int target = g->board[SQ(nr, nc)];
                    if (!IS_EMPTY(target)) {
                        if (PIECE_COLOR(target) != side)
                            add_move(moves, &count, sq, SQ(nr, nc), p, target, EMPTY, 0, 0);
                        break;
                    }
                    add_move(moves, &count, sq, SQ(nr, nc), p, EMPTY, EMPTY, 0, 0);
                    nr += slide[i][0];
                    nc += slide[i][1];
                }
            }
        }
    }

    return count;
}

/* ======================================================================
 * Make / Unmake move
 * ====================================================================== */

/* Castling right stripped when a rook leaves or is captured on `sq`. */
static int rook_right(int sq)
{
    if (sq == SQ(7, 0)) return WQ;
    if (sq == SQ(7, 7)) return WK;
    if (sq == SQ(0, 0)) return BQ;
    if (sq == SQ(0, 7)) return BK;
    return 0;
}

static void note_capture(Game *g, int piece)
{
    if (IS_WHITE(piece)) g->white_captured[PIECE_TYPE(piece)]++;
    else g->black_captured[PIECE_TYPE(piece)]++;
}

static void undo_capture(Game *g, int piece)
{
    int *count = IS_WHITE(piece) ? g->white_captured : g->black_captured;
    int type = PIECE_TYPE(piece);
    if (count[type] > 0)
        count[type]--;
}

/* Apply `m`, recording the previous state for unmake_move.  The Zobrist key is
 * updated here so every caller (game and search) can trust g->hash. */
static void make_move(Game *g, const Move *m)
{
    if (g->history_len >= MAX_UNDO)
        return;

    Move *entry = &g->history[g->history_len++];
    *entry = *m;
    entry->prev_ep = g->ep_square;
    entry->prev_castle = g->castle;
    entry->halfmove = g->halfmove_clock;
    entry->prev_hash = g->hash;

    int from = m->from, to = m->to;
    uint64_t key = g->hash ^ zobrist_side;

    if (g->ep_square >= 0)
        key ^= zobrist_ep[COL(g->ep_square)];
    key ^= zobrist_castle[g->castle];

    if (m->castle) {
        int rf = SQ(ROW(from), m->castle == 1 ? 7 : 0);
        int rt = SQ(ROW(from), m->castle == 1 ? 5 : 3);
        g->board[rt] = g->board[rf];
        g->board[rf] = EMPTY;
        key ^= piece_key(rf, g->board[rt]) ^ piece_key(rt, g->board[rt]);
    }

    if (m->en_passant) {
        int cap_sq = SQ(ROW(from), COL(to));
        int captured = g->board[cap_sq];
        g->board[cap_sq] = EMPTY;
        if (!IS_EMPTY(captured)) {
            key ^= piece_key(cap_sq, captured);
            note_capture(g, captured);
        }
    } else if (!IS_EMPTY(m->captured)) {
        key ^= piece_key(to, m->captured);
        note_capture(g, m->captured);
    }

    int placed = m->promoted != EMPTY ? PIECE(m->promoted, PIECE_COLOR(m->piece)) : m->piece;
    key ^= piece_key(from, m->piece) ^ piece_key(to, placed);
    g->board[from] = EMPTY;
    g->board[to] = placed;

    g->ep_square = PIECE_TYPE(m->piece) == PAWN && abs_val(ROW(to) - ROW(from)) == 2
                 ? SQ((ROW(from) + ROW(to)) / 2, COL(from)) : -1;
    if (g->ep_square >= 0)
        key ^= zobrist_ep[COL(g->ep_square)];

    if (PIECE_TYPE(m->piece) == KING)
        g->castle &= IS_WHITE(m->piece) ? ~(WK | WQ) : ~(BK | BQ);
    g->castle &= ~rook_right(from) & ~rook_right(to);
    key ^= zobrist_castle[g->castle];

    g->halfmove_clock = (PIECE_TYPE(m->piece) == PAWN || !IS_EMPTY(m->captured))
                      ? 0 : g->halfmove_clock + 1;
    g->hash = key;

    g->side = (g->side == WHITE) ? BLACK : WHITE;
    if (g->side == WHITE)
        g->fullmove++;
}

static void unmake_move(Game *g)
{
    if (g->history_len <= 0)
        return;
    Move m = g->history[--g->history_len];
    int from = m.from, to = m.to;

    g->side = (g->side == WHITE) ? BLACK : WHITE;
    if (g->side == BLACK)
        g->fullmove--;

    g->board[from] = m.piece;
    g->board[to] = m.captured;

    if (m.en_passant) {
        int cap_sq = SQ(ROW(from), COL(to));
        int captured = PAWN | (IS_WHITE(m.piece) ? BLACK : WHITE);
        g->board[cap_sq] = captured;
        g->board[to] = EMPTY;
        undo_capture(g, captured);
    } else if (!IS_EMPTY(m.captured)) {
        undo_capture(g, m.captured);
    }

    if (m.castle) {
        int rf = SQ(ROW(from), m.castle == 1 ? 7 : 0);
        int rt = SQ(ROW(from), m.castle == 1 ? 5 : 3);
        g->board[rf] = g->board[rt];
        g->board[rt] = EMPTY;
    }

    g->ep_square = m.prev_ep;
    g->castle = m.prev_castle;
    g->halfmove_clock = m.halfmove;
    g->hash = m.prev_hash;
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
        copy_position(&copy, g);
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

/* A pawn is passed when no enemy pawn stands ahead of it on its own or an
 * adjacent file. */
static int is_passed_pawn(const Game *g, int sq, int color)
{
    int r = ROW(sq), c = COL(sq);
    int enemy = PAWN | (color ? WHITE : BLACK);
    int dir = color ? 1 : -1;

    for (int i = r + dir; i >= 0 && i < 8; i += dir)
        for (int dc = -1; dc <= 1; dc++)
            if (c + dc >= 0 && c + dc < 8 && g->board[SQ(i, c + dc)] == enemy)
                return 0;
    return 1;
}

/* Danger score around a king: enemy material nearby, weak pawn shield and open
 * files.  Higher means more exposed. */
static int king_safety(const Game *g, int color)
{
    static const int weight[7] = {0, 0, 15, 8, 10, 20, 0};
    int king = KING | (color ? BLACK : WHITE);
    int enemy_color = color ? WHITE : BLACK;
    int king_sq = -1, sq;

    for (sq = 0; sq < 64; sq++)
        if (g->board[sq] == king) { king_sq = sq; break; }
    if (king_sq < 0)
        return 0;

    int r = ROW(king_sq), c = COL(king_sq);
    int penalty = 0;

    for (int nr = r - 2; nr <= r + 2; nr++)
        for (int nc = c - 2; nc <= c + 2; nc++)
            if (in_bounds(nr, nc) && PIECE_COLOR(g->board[SQ(nr, nc)]) == enemy_color)
                penalty += weight[PIECE_TYPE(g->board[SQ(nr, nc)])];

    int shield_row = r + (color ? 1 : -1);
    for (int nc = c - 1; nc <= c + 1; nc++)
        if (in_bounds(shield_row, nc) &&
            g->board[SQ(shield_row, nc)] == (PAWN | (color ? BLACK : WHITE)))
            penalty -= 8;

    for (int nc = c - 1; nc <= c + 1; nc++)
        if (nc >= 0 && nc < 8 && !has_pawn_on_file(g, color, nc))
            penalty += 10;

    return penalty;
}

/* One pseudo-legal pass per side gives both a mobility count and a bonus for
 * pawns/minors attacking something more valuable.  Scores are White-positive. */
static void compute_mobility_threats(const Game *g, int *mobility, int *threats)
{
    static const int value[7] = {0, VAL_PAWN, VAL_KNIGHT, VAL_BISHOP, VAL_ROOK, VAL_QUEEN, VAL_KING};
    Move moves[MAX_MOVES];
    Game probe;
    int moves_for[2], threats_for[2];

    copy_position(&probe, g);
    for (int side = 0; side < 2; side++) {
        int color = side == 0 ? WHITE : BLACK;
        probe.side = color;
        int count = gen_pseudo_legal(&probe, moves);
        moves_for[side] = count;
        threats_for[side] = 0;
        for (int i = 0; i < count; i++)
            if (!IS_EMPTY(moves[i].captured) &&
                value[PIECE_TYPE(moves[i].captured)] > value[PIECE_TYPE(moves[i].piece)])
                threats_for[side]++;
    }

    *mobility = (moves_for[0] - moves_for[1]) * 3;
    *threats = (threats_for[0] - threats_for[1]) * 10;
}

static int evaluate(const Game *g)
{
    int score = 0;
    int sq, p, pt, c;
    int white_pawns_on_file[8] = {0};
    int black_pawns_on_file[8] = {0};

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
            if (c == 0) white_pawns_on_file[COL(sq)]++;
            else black_pawns_on_file[COL(sq)]++;
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
            const int *pawns = c == 0 ? white_pawns_on_file : black_pawns_on_file;
            int sign = c == 0 ? 1 : -1;
            int neighbours = (file > 0 && pawns[file - 1]) +
                             (file < 7 && pawns[file + 1]);

            if (pawns[file] > 1) eval_pawn_structure -= sign * 15;
            if (neighbours == 0) eval_pawn_structure -= sign * 20;
            else if (neighbours == 1) eval_pawn_structure -= sign * 8;
            if (is_passed_pawn(g, sq, c)) {
                int adv = c == 0 ? ROW(sq) : 7 - ROW(sq);
                eval_pawn_structure += sign * (10 + adv * adv * 5);
            }
        }
        score += eval_pawn_structure;
    }

    if (g_difficulty >= DIFF_MEDIUM) {
        eval_king_safety = -king_safety(g, 0) + king_safety(g, 1);
        score += eval_king_safety;
    }

    if (g_difficulty >= DIFF_HARD) {
        static const int center_bonus[4] = {3, 5, 5, 3};
        static const int center_sq[4][2] = {{3,3},{3,4},{4,3},{4,4}};
        for (sq = 0; sq < 64; sq++) {
            p = g->board[sq];
            if (IS_EMPTY(p)) continue;
            c = IS_WHITE(p) ? 0 : 1;
            int sign = c == 0 ? 1 : -1;
            int r = ROW(sq), col = COL(sq);
            for (int k = 0; k < 4; k++) {
                if (r == center_sq[k][0] && col == center_sq[k][1]) {
                    eval_center += sign * center_bonus[k];
                    break;
                }
            }
        }
        score += eval_center;
    }

    {
        int mobility, threats;
        compute_mobility_threats(g, &mobility, &threats);
        eval_mobility = mobility;
        eval_threats = threats;
        score += eval_mobility;
        if (g_difficulty >= DIFF_HARD)
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
    static const int value[] = {0, 1, 3, 3, 5, 9, 100};
    int victim = PIECE_TYPE(m->captured);
    if (victim == 0)
        return 0;
    return value[victim] * 10 - value[PIECE_TYPE(m->piece)];
}

/* Sort by a cheap static score: TT move, captures (MVV-LVA), promotions, then
 * centralisation. */
static void order_moves(Move *moves, int count, int tt_move)
{
    int scores[MAX_MOVES];

    for (int i = 0; i < count; i++) {
        int r = ROW(moves[i].to), c = COL(moves[i].to);
        scores[i] = mvv_lva(&moves[i]) * 10;
        if (tt_move >= 0 &&
            moves[i].from == move_key_from(tt_move) && moves[i].to == move_key_to(tt_move))
            scores[i] += 1000000;
        if (moves[i].promoted != EMPTY) scores[i] += 500;
        if (moves[i].castle) scores[i] += 200;
        if (r >= 2 && r <= 5 && c >= 2 && c <= 5) scores[i] += 10;
    }

    for (int i = 0; i + 1 < count; i++) {
        int best = i;
        for (int j = i + 1; j < count; j++)
            if (scores[j] > scores[best])
                best = j;
        if (best != i) {
            Move tm = moves[i]; moves[i] = moves[best]; moves[best] = tm;
            int ts = scores[i]; scores[i] = scores[best]; scores[best] = ts;
        }
    }
}

/* Search only captures after the main search horizon, so the evaluation is not
 * fooled by a pending recapture. */
static int quiesce(Game *g, int alpha, int beta, int depth)
{
    int stand = evaluate(g);
    if (stand >= beta) return beta;
    if (stand > alpha) alpha = stand;
    if (depth <= 0) return alpha;

    Move moves[MAX_MOVES];
    int count = gen_legal(g, moves);
    order_moves(moves, count, -1);

    for (int i = 0; i < count; i++) {
        if (IS_EMPTY(moves[i].captured))
            continue;
        make_move(g, &moves[i]);
        int score = -quiesce(g, -beta, -alpha, depth - 1);
        unmake_move(g);
        if (score >= beta) return beta;
        if (score > alpha) alpha = score;
    }
    return alpha;
}

static int alpha_beta(Game *g, int depth, int alpha, int beta, int do_null)
{
    Move moves[MAX_MOVES];
    int count = gen_legal(g, moves);

    if (count == 0)
        return in_check(g, g->side) ? -INF - depth : 0; /* mate / stalemate */
    if (g->halfmove_clock >= 100)
        return 0; /* 50-move rule */

    int tt_move = -1, tt_score;
    if (tt_probe(g->hash, depth, alpha, beta, &tt_score, &tt_move))
        return tt_score;

    if (depth <= 0)
        return quiesce(g, alpha, beta, 6);

    /* Null move: if giving the opponent a free move still fails high, assume a
     * real move would too.  The key is patched by hand since no move is made. */
    if (do_null && depth >= 3 && !in_check(g, g->side)) {
        Game child;
        copy_position(&child, g);
        child.side = (g->side == WHITE) ? BLACK : WHITE;
        child.hash = g->hash ^ zobrist_side;
        if (g->ep_square >= 0)
            child.hash ^= zobrist_ep[COL(g->ep_square)];
        child.ep_square = -1;
        if (-alpha_beta(&child, depth - 4, -beta, -beta + 1, 0) >= beta)
            return beta;
    }

    order_moves(moves, count, tt_move);

    int best = -INF - 1, best_move = -1, flag = TT_UPPER;
    for (int i = 0; i < count; i++) {
        make_move(g, &moves[i]);
        int score;
        if (i == 0) {
            score = -alpha_beta(g, depth - 1, -beta, -alpha, 1);
        } else {
            /* PVS: verify the first move of every later branch with a null
             * window, re-searching only when it beats alpha. */
            score = -alpha_beta(g, depth - 1, -alpha - 1, -alpha, 1);
            if (score > alpha && score < beta)
                score = -alpha_beta(g, depth - 1, -beta, -alpha, 1);
        }
        unmake_move(g);

        if (score > best) {
            best = score;
            best_move = move_key(moves[i].from, moves[i].to);
        }
        if (score > alpha) {
            alpha = score;
            flag = TT_EXACT;
        }
        if (alpha >= beta) {
            flag = TT_LOWER;
            break;
        }
    }

    tt_store(g->hash, depth, best, flag, best_move);
    return best;
}

/* Iterative deepening: search depth 1, 2, ... keeping the best root move so a
 * later iteration can order it first (and so a timeout would still have a
 * usable result). */
static Move find_best_move(Game *g, int max_depth)
{
    Move moves[MAX_MOVES];
    int count = gen_legal(g, moves);

    if (count == 0) {
        Move none = {0};
        none.from = none.to = -1;
        return none;
    }
    if (count == 1)
        return moves[0];

    Move best = moves[0];
    for (int d = 1; d <= max_depth; d++) {
        int tt_move = -1, score;
        tt_probe(g->hash, d, -INF, INF, &score, &tt_move);
        order_moves(moves, count, tt_move);

        int best_score = -INF - 1, best_idx = -1;
        for (int i = 0; i < count; i++) {
            make_move(g, &moves[i]);
            if (i == 0) {
                score = -alpha_beta(g, d - 1, -INF, -best_score, 1);
            } else {
                score = -alpha_beta(g, d - 1, -best_score - 1, -best_score, 1);
                if (score > best_score)
                    score = -alpha_beta(g, d - 1, -INF, -best_score, 1);
            }
            unmake_move(g);
            if (score > best_score) {
                best_score = score;
                best_idx = i;
            }
        }
        if (best_idx >= 0)
            best = moves[best_idx];
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

            /* Background color.  Both square colours are kept light so that
             * pieces drawn in dark foreground colours stay readable. */
            if (is_king_check)
                printf("\033[41m"); /* red for check */
            else if (is_last)
                printf("\033[43m"); /* yellow for last move */
            else if (is_light)
                printf("\033[47m"); /* white light square */
            else
                printf("\033[46m"); /* cyan dark square */

            /* Foreground color.  The old scheme drew white pieces bright white
             * on a white square, which made them invisible.  Use two dark
             * colours against the light squares instead, and bright white for
             * the checked king on its red square. */
            if (is_king_check)
                printf("\033[1;97m");
            else if (!IS_EMPTY(p) && IS_WHITE(p))
                printf("\033[1;31m"); /* white pieces: bold red */
            else if (!IS_EMPTY(p))
                printf("\033[1;30m"); /* black pieces: bold black */

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
    printf("  \033[33mpgn\033[0m        - export the game to chess.pgn\r\n");
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
            if (legal[i].promoted == promo || promo == EMPTY) {
                *out = legal[i];
                return 0;
            }
        }
    }

    return -1;
}

/* Return the square the player picks from stdin, or -1 to cancel.  Promotions
 * need an extra choice the mouse cannot express, so they are asked here. */
static int choose_promotion(void)
{
    static const char letters[4] = {'q', 'r', 'b', 'n'};
    static const int types[4] = {QUEEN, ROOK, BISHOP, KNIGHT};

    printf("  Promote to (q)ueen (r)ook (b)ishop k(n)ight: ");
    fflush(stdout);
    int ch = fgetc(stdin);
    while (ch != '\n' && ch != '\r' && ch != -1) {
        for (int i = 0; i < 4; i++) {
            if (ch == letters[i] || ch == (letters[i] - 'a' + 'A')) {
                while (ch != '\n' && ch != '\r' && ch != -1)
                    ch = fgetc(stdin);
                return types[i];
            }
        }
        ch = fgetc(stdin);
    }
    return -1;
}

/* Complete a mouse move.  `m` is the base move found for sq->to; if it is a
 * promotion the player is prompted and `m` is refined to the chosen piece. */
static int resolve_promotion(Game *g, Move *m)
{
    if (m->promoted == EMPTY)
        return 0;
    int piece = choose_promotion();
    if (piece == EMPTY)
        return -1;
    Move legal[MAX_MOVES];
    int n = gen_legal(g, legal);
    for (int i = 0; i < n; i++)
        if (legal[i].from == m->from && legal[i].to == m->to && legal[i].promoted == piece) {
            *m = legal[i];
            return 0;
        }
    return -1;
}

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
    char *dest;
    int idx = 0;
    char cols[] = "abcdefgh";

    /* Defensive: never index past the notation array. */
    if (g->notation_count < 0 || g->notation_count >= MAX_UNDO)
        return;
    dest = g->move_notation[g->notation_count];

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
 * PGN export
 * ====================================================================== */

/* Natural move text for move `i` (a half-move).  Check/mate needs the position
 * after the move; disambiguation needs every square a same-type piece could
 * have reached the destination from. */
static void san_append(const Game *g, int i, char *out)
{
    const char *note = g->move_notation[i];
    int side = PIECE_COLOR(g->history[i].piece);
    int type = PIECE_TYPE(g->history[i].piece);
    int to = g->history[i].to;
    int len = 0;

    while (note[len] && len < 6) {
        out[len] = note[len];
        len++;
    }

    if (in_check(g, g->side)) {
        Move replies[MAX_MOVES];
        out[len++] = gen_legal(g, replies) == 0 ? '#' : '+';
    }
    out[len] = '\0';

    /* Long algebraic already includes from-file for pawns and captures. */
    if (type == PAWN || strchr(note, 'x'))
        return;

    /* Count same-type pieces that could also move to `to`. */
    int rivals = 0, same_file = 0;
    for (int sq = 0; sq < 64; sq++) {
        if (sq == g->history[i].from || g->board[sq] != (type | side))
            continue;
        if (!piece_reaches(g, sq, to))
            continue;
        rivals++;
        if (COL(sq) == COL(g->history[i].from))
            same_file = 1;
    }
    if (rivals == 0)
        return;

    char hint = same_file ? '0' + (8 - ROW(g->history[i].from)) : 'a' + COL(g->history[i].from);
    memmove(out + 2, out + 1, len - 1);
    out[1] = hint;
}

static int export_pgn(const Game *g, const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        printf("  \033[31mCannot write '%s'.\033[0m\r\n", path);
        return -1;
    }

    fprintf(f, "[Event \"A20OS Chess\"]\r\n");
    fprintf(f, "[Site \"A20OS\"]\r\n");
    fprintf(f, "[White \"A20OS AI (%s)\"]\r\n", difficulty_name(g_difficulty));
    fprintf(f, "[Black \"Human\"]\r\n");
    fprintf(f, "[Result \"*\"]\r\n\r\n");

    int line = 0;
    for (int i = 0; i < g->notation_count; i++) {
        char san[12];
        san_append(g, i, san);
        if (i % 2 == 0)
            line += fprintf(f, "%d. ", i / 2 + 1);
        line += fprintf(f, "%s ", san);
        if (line >= 76) {
            fprintf(f, "\r\n");
            line = 0;
        }
    }
    fprintf(f, "*\r\n");
    fclose(f);

    printf("  PGN written to \033[33m%s\033[0m (%d plies)\r\n\r\n", path, g->notation_count);
    return 0;
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
                    if (parse_move(move_str, &game, &m) == 0 && resolve_promotion(&game, &m) == 0) {
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
        if (strcmp(input, "pgn") == 0) { export_pgn(&game, "chess.pgn"); continue; }
        if (strcmp(input, "undo") == 0) {
            if (game.history_len >= 2) {
                unmake_move(&game);
                unmake_move(&game);
                if (game.notation_count >= 2) game.notation_count -= 2;
                else game.notation_count = 0;
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
            if (resolve_promotion(&game, &m) < 0) {
                printf("  Promotion cancelled.\r\n");
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
