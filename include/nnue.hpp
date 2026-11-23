#pragma once
#include "micro.hpp"
#include <cassert>
#include <type_traits>

namespace micro {
template<class Weight, int Features, int Hidden, int Buckets = 1>
struct NnueWeights {
    static_assert(std::is_same<Weight, int8_t>::value || std::is_same<Weight, int16_t>::value,
                  "Expected signed 8-bit or 16-bit weights");
    static_assert(Features == 768 || Features == 704, "Unsupported features");
    static_assert(Hidden > 0 && Hidden <= 256, "Unsupported width");
    static_assert(Buckets == 1 || Buckets == 8, "Unsupported buckets");
    Weight input[Features][Hidden];
    int32_t bias[Hidden];
    Weight output[Buckets][2][Hidden];
    int64_t outputBias[Buckets];
    int32_t activationScale;
    alignas(8) int64_t divisor;
};

template<class Weight, int Features, int Hidden, int Buckets = 1>
class Nnue {
    using Model = NnueWeights<Weight, Features, Hidden, Buckets>;
    const Model& model;
    unsigned outputShift = 0;
    int32_t sum[2][Hidden];
    int count = 0;
    static int mirror(int king) { return Features == 704 && (king & 4) ? 7 : 0; }
    void add(int view, int piece, int square, int fileMask, int direction) {
        int channel = (color(piece) ^ view) * 6 + type(piece) - 1;
        int sq = index64(square) ^ (view ? 56 : 0) ^ fileMask;
        int feature = channel * 64 + sq;
        if (Features == 704) {
            feature -= (channel / 6) * 32;
            if (type(piece) == King)
                feature = channel * 64 - (channel / 6) * 32 +
                    (sq / 8) * 4 + ((sq & 4) ? 7 - (sq & 7) : sq & 7);
        }
        const auto& row = model.input[feature];
        for (int i = 0; i < Hidden; ++i) sum[view][i] += direction * row[i];
    }
    void refreshView(const Board& b, int view) {
        for (int i = 0; i < Hidden; ++i) sum[view][i] = model.bias[i];
        int fileMask = mirror(b.king[view]);
        for (int rank = 0; rank < 128; rank += 16)
            for (int sq = rank; sq < rank + 8; ++sq)
                if (b.square[sq]) add(view, b.square[sq], sq, fileMask, 1);
    }
    void transition(const Board& child, Move move, const Undo& undo, int direction) {
        int from = move.from(), to = move.to();
        int placed = child.square[to], mover = color(placed);
        int piece = move.promotion() ? sign(mover) * Pawn : placed;
        int capture = type(piece) == Pawn && to == undo.ep ? to - 16 * sign(mover) : to;
        bool castle = type(piece) == King && (to - from == 2 || from - to == 2);
        int rook = sign(mover) * Rook;
        for (int view = 0; view < 2; ++view) {
            int fileMask = mirror(child.king[view]);
            if (view == mover && type(piece) == King && fileMask != mirror(from)) {
                if (direction > 0) {
                    refreshView(child, view);
                    continue;
                }
                Board parent = child;
                parent.unmake(move, undo);
                refreshView(parent, view);
                continue;
            }
            add(view, piece, from, fileMask, -direction);
            add(view, placed, to, fileMask, direction);
            if (undo.captured) add(view, undo.captured, capture, fileMask, -direction);
            if (castle) {
                add(view, rook, to > from ? from + 3 : from - 4, fileMask, -direction);
                add(view, rook, (from + to) / 2, fileMask, direction);
            }
        }
        if (undo.captured) count -= direction;
    }
    static int score(const Board& b, void* ctx) {
        int value = static_cast<Nnue*>(ctx)->output(b.side);
        constexpr int limit = Mate - MaxPly - 1;
        return value < -limit ? -limit : value > limit ? limit : value;
    }
    static void push(const Board& b, Move m, const Undo& u, void* ctx) {
        static_cast<Nnue*>(ctx)->transition(b, m, u, 1);
    }
    static void pop(const Board& b, Move m, const Undo& u, void* ctx) {
        static_cast<Nnue*>(ctx)->transition(b, m, u, -1);
    }
public:
    explicit Nnue(const Model& weights) : model(weights) {
        assert(model.divisor > 0 && (model.divisor & (model.divisor - 1)) == 0);
        for (auto divisor = model.divisor; divisor > 1; divisor >>= 1) ++outputShift;
    }
    void refresh(const Board& b) {
        count = 0;
        for (int rank = 0; rank < 128; rank += 16)
            for (int sq = rank; sq < rank + 8; ++sq) count += b.square[sq] != 0;
        refreshView(b, White);
        refreshView(b, Black);
    }
    bool sameState(const Nnue& other) const {
        if (count != other.count) return false;
        for (int view = 0; view < 2; ++view)
            for (int i = 0; i < Hidden; ++i)
                if (sum[view][i] != other.sum[view][i]) return false;
        return true;
    }
    int output(int side) const {
        int bucket = Buckets == 1 ? 0 : (count - 1) / 4;
        if (bucket >= Buckets) bucket = Buckets - 1;
        int64_t value = model.outputBias[bucket];
        for (int order = 0; order < 2; ++order)
            for (int i = 0; i < Hidden; ++i) {
                int32_t x = sum[side ^ order][i];
                x = x < 0 ? 0 : x > model.activationScale ? model.activationScale : x;
                value += x * int32_t(model.output[bucket][order][i]);
            }
        const int64_t half = model.divisor >> 1;
        return value < 0 ? -int((-value + half) >> outputShift)
                         : int((value + half) >> outputShift);
    }
    Evaluator evaluator() { return {score, this, push, pop}; }
};
}
