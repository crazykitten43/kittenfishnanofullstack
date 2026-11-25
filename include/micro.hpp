#pragma once
#include <cstddef>
#include <cstdint>

namespace micro {
constexpr char Start[] = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
enum Piece { Empty, Pawn, Knight, Bishop, Rook, Queen, King };
enum Side { White, Black };
inline int color(int p) { return p < 0 ? Black : White; }
inline int type(int p) { return p < 0 ? -p : p; }
inline int sign(int side) { return side == White ? 1 : -1; }
inline bool valid(int sq) { return (sq & 0x88) == 0; }
inline int index64(int sq) { return (sq >> 4) * 8 + (sq & 7); }

struct Move {
    uint16_t code = 0;
    Move() = default;
    Move(int from, int to, int promote = 0)
        : code(uint16_t(index64(from) | (index64(to) << 6) | (promote << 12))) {}
    int from() const { return (code & 7) | ((code & 56) << 1); }
    int to() const { return ((code >> 6) & 7) | ((code >> 5) & 112); }
    int promotion() const { return code >> 12; }
    explicit operator bool() const { return code != 0; }
};
constexpr int MaxMoves = 320;
struct Moves {
    Move list[MaxMoves];
    int size = 0;
    void add(Move move) { list[size++] = move; }
};
struct Undo {
    uint64_t key;
    uint16_t halfmove;
    uint8_t rights;
    int8_t ep, captured;
};
struct Board {
    int8_t square[128]{};
    uint64_t key = 0, pawnKey = 0;
    uint64_t pieces[2][7]{};
    int material = 0, mg = 0, eg = 0, phase = 0;
    int king[2]{};
    int side = White, rights = 0, ep = -1;
    uint16_t halfmove = 0;

    bool load(const char* fen);
    bool attacked(int sq, int by) const;
    bool check() const { return attacked(king[side], side ^ 1); }
    void generate(Moves& moves, bool noisy = false) const;
    bool make(Move move, Undo& undo);
    void unmake(Move move, const Undo& undo);
    bool dead() const;
    int epFile() const;
private:
    void put(int sq, int p);
    bool parse(const char* fen);
};

void notation(Move move, char out[6]);
Move parseMove(Board& board, const char* text);
uint64_t perft(Board& board, int depth);

// Only non-power-of-two tables need a division. Fold for 32-bit targets.
inline size_t tableIndex(uint64_t key,size_t n) {
    uint32_t folded=uint32_t(key) ^ uint32_t(key>>32);
    return (n&(n-1)) ? folded%n : folded&(n-1);
}

struct PawnEntry { uint64_t key; int score; bool valid; };
struct Hce {
    PawnEntry* cache = nullptr;
    size_t capacity = 0;
    int evaluate(const Board& board) const;
    static int call(const Board& board, void* context);
};
struct Evaluator {
    int (*score)(const Board&, void*);
    void* context;
    // The child hook receives the board after make and before unmake.
    void (*push)(const Board&, Move, const Undo&, void*) = nullptr;
    void (*pop)(const Board&, Move, const Undo&, void*) = nullptr;
};

constexpr int Mate = 30000, MaxPly = 48;
struct TTEntry { uint64_t key; uint16_t move; int16_t score; int8_t depth; uint8_t bound; };
struct Limits { int milliseconds = 0, depth = 32; uint64_t nodes = 0; };
struct Result { Move move; int score = 0, depth = 0; uint64_t nodes = 0; };
using Clock = uint64_t (*)();
enum class SearchProfile { SelectivePvs, QuietPruning, LateMoveReductions, AspirationPvs };

enum SearchOptimization : unsigned {
    QuietHistory = 1, FastStandPat = 2, QuiescenceTable = 4,
    AdaptiveLmr = 8, DeltaPruning = 16, WidenAspiration = 32
};

class Search {
public:
    Search(Evaluator eval, TTEntry* table, size_t entries, Clock clock)
        : eval_(eval), table_(table), entries_(entries), clock_(clock) {}
    Result run(Board& board, Limits limits, const uint64_t* previous = nullptr, size_t count = 0);
    void clear();
    void setOptimizations(unsigned mask) { optimizations_ = mask; clear(); }
    void setProfile(SearchProfile profile) { profile_ = profile; clear(); }
    void setEvaluator(Evaluator eval) { eval_ = eval; clear(); }
private:
    int negamax(Board&, int depth, int ply, int alpha, int beta);
    bool stop();
    SearchProfile profile_ = SearchProfile::SelectivePvs;
    unsigned optimizations_ = 0;
    Evaluator eval_;
    TTEntry* table_;
    size_t entries_;
    Clock clock_;
    uint64_t nodes_, deadline_, keys_[MaxPly]{};
    const uint64_t* previous_;
    size_t previousCount_;
    Limits limits_;
    bool stopped_;
    Move root_;
    Move killers_[MaxPly][2]{};
    int16_t quietHistory_[2][King+1][64]{};
    struct Frame { Moves moves; int16_t order[MaxMoves]; };
    Frame frames_[MaxPly];
    uint64_t history_[MaxPly]{};
};
}
