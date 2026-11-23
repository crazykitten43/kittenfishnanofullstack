#ifdef ARDUINO
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <cstdlib>
#include <cstring>
#include <new>
#include "micro.hpp"
#include "nnue_704x128_int16.hpp"

#ifndef KFM_TT_KIB
#define KFM_TT_KIB 256
#endif

#ifdef SET_LOOP_TASK_STACK_SIZE
SET_LOOP_TASK_STACK_SIZE(16 * 1024);
#endif

namespace {
using namespace micro;
constexpr size_t capacity=128;
Board board;
uint64_t history[capacity]{};
size_t count=0;
TTEntry internalTable[1024]{};
PawnEntry pawnCache[256]{};
Hce hce{pawnCache,256};
ExportedNnue network(exportedWeights);
bool networkActive=false;
alignas(Search) uint8_t searchStorage[sizeof(Search)];
Search* engine;
char line[128]{};
size_t length=0;
bool overflow=false;

uint64_t clockMicros() {
    static uint64_t lastYield=0;
    auto now=uint64_t(esp_timer_get_time());
    if (now-lastYield>=250000) {
        delay(1);
        lastYield=now=uint64_t(esp_timer_get_time());
    }
    return now;
}

void command(char* text) {
    if (!std::strcmp(text,"start")) {
        board.load(Start);
        count=0;
        engine->clear();
        Serial.println("ok");
    } else if (!std::strncmp(text,"fen ",4)) {
        if (!board.load(text+4)) { Serial.println("error fen"); return; }
        count=0;
        engine->clear();
        Serial.println("ok");
    } else if (!std::strncmp(text,"play ",5)) {
        Move move=parseMove(board,text+5);
        Undo u;
        if (!move || !board.make(move,u)) { Serial.println("error move"); return; }
        if (!board.halfmove) count=0;
        else {
            if (count==capacity) {
                std::memmove(history,history+1,(capacity-1)*sizeof(uint64_t));
                --count;
            }
            history[count++]=u.key;
        }
        Serial.println("ok");
    } else if (!std::strcmp(text,"hce 4")) {
        networkActive=false;
        engine->setEvaluator({Hce::call,&hce});
        Serial.println("ok");
    } else if (!std::strcmp(text,"nnue")) {
        networkActive=true;
        engine->setEvaluator(network.evaluator());
        Serial.println("ok");
    } else if (!std::strncmp(text,"search ",7)) {
        const char* name=text+7;
        SearchProfile profile;
        if (!std::strcmp(name,"selectivepvs")) profile=SearchProfile::SelectivePvs;
        else if (!std::strcmp(name,"quietpruning")) profile=SearchProfile::QuietPruning;
        else if (!std::strcmp(name,"latemovereductions")) profile=SearchProfile::LateMoveReductions;
        else if (!std::strcmp(name,"aspirationpvs")) profile=SearchProfile::AspirationPvs;
        else { Serial.println("error search"); return; }
        engine->setProfile(profile);
        Serial.println("ok");
    } else if (!std::strncmp(text,"go ",3)) {
        char* end;
        long ms=std::strtol(text+3,&end,10);
        if (end==text+3 || *end || ms<1 || ms>60000) { Serial.println("error time"); return; }
        if (networkActive) network.refresh(board);
        Result result=engine->run(board,{int(ms),32},history,count);
        char move[6];
        notation(result.move,move);
        Serial.printf("bestmove %s score %d depth %d nodes %llu\n",move,result.score,result.depth,
                      static_cast<unsigned long long>(result.nodes));
    } else Serial.println("error command");
}
}

void setup() {
    Serial.begin(115200);
    constexpr size_t externalEntries=size_t(KFM_TT_KIB)*1024/sizeof(TTEntry);
    auto* external=externalEntries ? static_cast<TTEntry*>(
        heap_caps_calloc(externalEntries,sizeof(TTEntry),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)) : nullptr;
    engine=new (searchStorage) Search({Hce::call,&hce},external ? external : internalTable,
                                    external ? externalEntries : 1024,clockMicros);
    board.load(Start);
    Serial.printf("KittenFish Nano Fullstack tt_bytes %u storage %s board_bytes %u "
                  "search_bytes %u pawn_cache_bytes %u\n",
                  unsigned((external ? externalEntries : 1024)*sizeof(TTEntry)),
                  external ? "psram" : "internal", unsigned(sizeof(Board)),
                  unsigned(sizeof(Search)), unsigned(sizeof(pawnCache)));
}

void loop() {
    while (Serial.available()) {
        int ch=Serial.read();
        if (ch=='\r') continue;
        if (ch=='\n') {
            if (overflow) Serial.println("error line-too-long");
            else if (length) {
                line[length]=0;
                command(line);
            }
            length=0;
            overflow=false;
        } else if (length+1<sizeof(line)) line[length++]=char(ch);
        else overflow=true;
    }
    delay(1);
}
#endif
