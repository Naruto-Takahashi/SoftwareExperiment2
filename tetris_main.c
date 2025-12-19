/* ===================================================================
 * tetris_main.c
 * テーマ3対応: イベント駆動型マルチタスク
 * 修正版: 
 * 1. 起動時の乱数固定化問題を修正 (wait_start関数の追加)
 * 2. 描画最適化 & ノンブロッキングアニメーション (維持)
 * =================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mtk_c.h"

/* -------------------------------------------------------------------
 * ファイルポインタ参照
 * ------------------------------------------------------------------- */
extern FILE *com0in;
extern FILE *com0out;
extern FILE *com1in;
extern FILE *com1out;

/* -------------------------------------------------------------------
 * 外部関数
 * ------------------------------------------------------------------- */
extern void init_kernel(void);
extern void set_task(void (*func)());
extern void begin_sch(void);
extern int inbyte(int ch);
extern void skipmt(void);

/* tick は mtk_c.c で定義されているタイマーカウンタ (extern宣言が必要) */
extern volatile unsigned long tick; 

/* -------------------------------------------------------------------
 * 定数・マクロ
 * ------------------------------------------------------------------- */
#define FIELD_WIDTH  12
#define FIELD_HEIGHT 22
#define MINO_WIDTH   4
#define MINO_HEIGHT  4

/* 相手の画面を表示する際の左端からのオフセット(文字数) */
#define OPPONENT_OFFSET_X 40 

/* 落下スピード (tick) */
#define DROP_INTERVAL 1000

/* アニメーションの長さ (tick) */
#define ANIMATION_DURATION 50

/* 画面更新の間引き (重さ対策) */
#define DISPLAY_POLL_INTERVAL 50

/* VT100 エスケープシーケンス */
#define ESC_CLS      "\x1b[2J"
#define ESC_HOME     "\x1b[H"
#define ESC_RESET    "\x1b[0m"
#define ESC_HIDE_CUR "\x1b[?25l"
#define ESC_SHOW_CUR "\x1b[?25h"
#define ESC_CLR_LINE "\x1b[K"

/* 画面反転演出用 */
#define ESC_INVERT_ON  "\x1b[?5h"
#define ESC_INVERT_OFF "\x1b[?5l"

/* カラー定義 (TrueColor) */
#define COL_CYAN     "\x1b[38;2;0;255;255m"
#define COL_YELLOW   "\x1b[38;2;255;255;0m"
#define COL_PURPLE   "\x1b[38;2;160;32;240m"
#define COL_BLUE     "\x1b[38;2;0;0;255m"
#define COL_ORANGE   "\x1b[38;2;255;165;0m"
#define COL_GREEN    "\x1b[38;2;0;255;0m"
#define COL_RED      "\x1b[38;2;255;0;0m"
#define COL_WHITE    "\x1b[38;2;255;255;255m"
#define COL_GRAY     "\x1b[38;2;128;128;128m"
#define BG_BLACK     "\x1b[40m"
#define COL_WALL     COL_WHITE

/* -------------------------------------------------------------------
 * ゲームの状態定義
 * ------------------------------------------------------------------- */
typedef enum {
    GS_PLAYING,     /* 通常プレイ中 */
    GS_ANIMATING,   /* ライン消去演出中 */
    GS_GAMEOVER     /* ゲームオーバー */
} GameState;

typedef enum {
    EVT_NONE,
    EVT_KEY_INPUT,
    EVT_TIMER,
    EVT_WIN,
    EVT_QUIT,
    EVT_REFRESH
} EventType;

typedef struct {
    EventType type;
    int param;
} Event;

/* -------------------------------------------------------------------
 * ゲーム状態構造体
 * ------------------------------------------------------------------- */
typedef struct {
    int port_id;
    FILE *fp_out;
    
    char field[FIELD_HEIGHT][FIELD_WIDTH];
    char displayBuffer[FIELD_HEIGHT][FIELD_WIDTH];
    char prevBuffer[FIELD_HEIGHT][FIELD_WIDTH];
    
    char prevOpponentBuffer[FIELD_HEIGHT][FIELD_WIDTH]; 
    int opponent_was_connected;

    /* 現在のゲーム状態 */
    GameState state;
    unsigned long anim_start_tick;
    int lines_to_clear;

    /* ミノ情報 */
    int minoType;
    int minoAngle;
    int minoX;
    int minoY;
    
    /* 7-Bag システム */
    int bag[7];
    int bag_index;

    unsigned long next_drop_time;
    
    /* 入力解析用ステート */
    int seq_state;
    
    int score;
    int lines_cleared;
    
    volatile int pending_garbage;
    volatile int is_gameover;

    /* リトライ同期用 */
    volatile int sync_generation;

} TetrisGame;

TetrisGame *all_games[2] = {NULL, NULL};

/* -------------------------------------------------------------------
 * グローバルデータ (ミノ定義)
 * ------------------------------------------------------------------- */
enum { MINO_TYPE_I, MINO_TYPE_O, MINO_TYPE_S, MINO_TYPE_Z, MINO_TYPE_J, MINO_TYPE_L, MINO_TYPE_T, MINO_TYPE_GARBAGE, MINO_TYPE_MAX };
const char* minoColors[MINO_TYPE_MAX] = { COL_CYAN, COL_YELLOW, COL_GREEN, COL_RED, COL_BLUE, COL_ORANGE, COL_PURPLE, COL_GRAY };
enum { MINO_ANGLE_0, MINO_ANGLE_90, MINO_ANGLE_180, MINO_ANGLE_270, MINO_ANGLE_MAX };

/* ミノ形状データ (0/1) */
char minoShapes[MINO_TYPE_MAX][MINO_ANGLE_MAX][MINO_HEIGHT][MINO_WIDTH] = {
    /* MINO_TYPE_I */
    {
        {
            {0, 1, 0, 0},
            {0, 1, 0, 0},
            {0, 1, 0, 0},
            {0, 1, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 0, 0, 0},
            {1, 1, 1, 1},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 1, 0},
            {0, 0, 1, 0},
            {0, 0, 1, 0},
            {0, 0, 1, 0}
        },
        {
            {0, 0, 0, 0},
            {1, 1, 1, 1},
            {0, 0, 0, 0},
            {0, 0, 0, 0}
        }
    },
    /* MINO_TYPE_O */
    {
        {
            {0, 0, 0, 0},
            {0, 1, 1, 0},
            {0, 1, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 1, 0},
            {0, 1, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 1, 0},
            {0, 1, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 1, 0},
            {0, 1, 1, 0},
            {0, 0, 0, 0}
        }
    },
    /* MINO_TYPE_S */
    {
        {
            {0, 0, 0, 0},
            {0, 1, 1, 0},
            {1, 1, 0, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 1, 0, 0},
            {0, 1, 1, 0},
            {0, 0, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 0, 1, 1},
            {0, 1, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 0, 0},
            {0, 1, 1, 0},
            {0, 0, 1, 0}
        }
    },
    /* MINO_TYPE_Z */
    {
        {
            {0, 0, 0, 0},
            {1, 1, 0, 0},
            {0, 1, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 1, 0},
            {0, 1, 1, 0},
            {0, 1, 0, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 1, 0},
            {0, 0, 1, 1},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 0, 1, 0},
            {0, 1, 1, 0},
            {0, 1, 0, 0}
        }
    },
    /* MINO_TYPE_J */
    {
        {
            {0, 0, 1, 0},
            {0, 0, 1, 0},
            {0, 1, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 0, 0},
            {0, 1, 1, 1},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 1, 0},
            {0, 1, 0, 0},
            {0, 1, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {1, 1, 1, 0},
            {0, 0, 1, 0},
            {0, 0, 0, 0}
        }
    },
    /* MINO_TYPE_L */
    {
        {
            {0, 1, 0, 0},
            {0, 1, 0, 0},
            {0, 1, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 1, 1},
            {0, 1, 0, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 1, 0},
            {0, 0, 1, 0},
            {0, 0, 1, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 0, 1, 0},
            {1, 1, 1, 0},
            {0, 0, 0, 0}
        }
    },
    /* MINO_TYPE_T */
    {
        {
            {0, 0, 0, 0},
            {1, 1, 1, 0},
            {0, 1, 0, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 1, 0},
            {0, 1, 1, 0},
            {0, 0, 1, 0},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 0, 1, 0},
            {0, 1, 1, 1},
            {0, 0, 0, 0}
        },
        {
            {0, 0, 0, 0},
            {0, 1, 0, 0},
            {0, 1, 1, 0},
            {0, 1, 0, 0}
        }
    },
    /* Garbage */
    { {{0}},{{0}},{{0}},{{0}} }
};

/* -------------------------------------------------------------------
 * 描画ヘルパー関数
 * ------------------------------------------------------------------- */
void print_cell_content(FILE *fp, char cellVal) {
    if (cellVal == 0) {
        fprintf(fp, "%s・%s", BG_BLACK, ESC_RESET);
    } else if (cellVal == 1) {
        fprintf(fp, "%s%s■%s", BG_BLACK, COL_WALL, ESC_RESET);
    } else if (cellVal >= 2 && cellVal <= 9) {
        fprintf(fp, "%s%s■%s", BG_BLACK, minoColors[cellVal - 2], ESC_RESET);
    } else {
        fprintf(fp, "??");
    }
}

/* -------------------------------------------------------------------
 * display: 画面描画関数 (最適化版)
 * ------------------------------------------------------------------- */
void display(TetrisGame *game) {
    int i, j;
    int changes = 0;
    
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    TetrisGame *opponent = all_games[opponent_id];

    int opponent_connected = (opponent != NULL);
    if (opponent_connected && !game->opponent_was_connected) {
        memset(game->prevOpponentBuffer, -1, sizeof(game->prevOpponentBuffer));
    }
    game->opponent_was_connected = opponent_connected;

    memcpy(game->displayBuffer, game->field, sizeof(game->field));
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (game->minoType != MINO_TYPE_GARBAGE && 
                minoShapes[game->minoType][game->minoAngle][i][j]) {
                if (game->minoY + i >= 0 && game->minoY + i < FIELD_HEIGHT &&
                    game->minoX + j >= 0 && game->minoX + j < FIELD_WIDTH) {
                    game->displayBuffer[game->minoY + i][game->minoX + j] = 2 + game->minoType;
                }
            }
        }
    }

    fprintf(game->fp_out, "\x1b[1;1H");
    fprintf(game->fp_out, "[YOU] SC:%-5d LN:%-3d ATK:%d", 
            game->score, game->lines_cleared, game->pending_garbage);
    
    if (opponent != NULL) {
        fprintf(game->fp_out, "\x1b[1;%dH", OPPONENT_OFFSET_X);
        fprintf(game->fp_out, "[RIVAL] SC:%-5d LN:%-3d", 
                opponent->score, opponent->lines_cleared);
    } else {
        fprintf(game->fp_out, "\x1b[1;%dH", OPPONENT_OFFSET_X);
        fprintf(game->fp_out, "[RIVAL] (Waiting...)    ");
    }
    fprintf(game->fp_out, "%s", ESC_CLR_LINE);

    fprintf(game->fp_out, "\n--------------------------");
    if (opponent != NULL) {
        fprintf(game->fp_out, "\x1b[2;%dH", OPPONENT_OFFSET_X);
        fprintf(game->fp_out, "--------------------------");
    }
    fprintf(game->fp_out, "%s", ESC_CLR_LINE);

    int base_y = 3;

    for (i = 0; i < FIELD_HEIGHT; i++) {
        /* 左側: 自分 */
        for (j = 0; j < FIELD_WIDTH; j++) {
            char myVal = game->displayBuffer[i][j];
            if (myVal != game->prevBuffer[i][j]) {
                fprintf(game->fp_out, "\x1b[%d;%dH", base_y + i, j * 2 + 1);
                print_cell_content(game->fp_out, myVal);
                game->prevBuffer[i][j] = myVal;
                changes++;
            }
        }

        /* 右側: 相手 */
        if (opponent != NULL) {
            for (j = 0; j < FIELD_WIDTH; j++) {
                char oppVal = opponent->displayBuffer[i][j];
                if (oppVal != game->prevOpponentBuffer[i][j]) {
                    fprintf(game->fp_out, "\x1b[%d;%dH", base_y + i, OPPONENT_OFFSET_X + j * 2);
                    print_cell_content(game->fp_out, oppVal);
                    game->prevOpponentBuffer[i][j] = oppVal;
                    changes++;
                }
            }
        }
    }
    
    if (changes > 0) fflush(game->fp_out);
}

/* -------------------------------------------------------------------
 * wait_event: イベント処理
 * ------------------------------------------------------------------- */
Event wait_event(TetrisGame *game) {
    Event e;
    e.type = EVT_NONE;
    int c;
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    static int poll_counter = 0;

    while (1) {
        if (all_games[opponent_id] != NULL && all_games[opponent_id]->is_gameover) {
            e.type = EVT_WIN;
            return e;
        }

        c = inbyte(game->port_id);
        if (c != -1) {
            if (game->seq_state == 0) {
                if (c == 0x1b) game->seq_state = 1;
                else if (c == 'q') { e.type = EVT_QUIT; return e; }
                else { e.type = EVT_KEY_INPUT; e.param = c; return e; }
            } 
            else if (game->seq_state == 1) {
                if (c == '[') game->seq_state = 2;
                else game->seq_state = 0;
            } 
            else if (game->seq_state == 2) {
                game->seq_state = 0;
                switch (c) {
                    case 'A': e.param = 'w'; break;
                    case 'B': e.param = 's'; break;
                    case 'C': e.param = 'd'; break;
                    case 'D': e.param = 'a'; break;
                    default:  e.param = 0;   break;
                }
                if (e.param != 0) { e.type = EVT_KEY_INPUT; return e; }
            }
        } 
        else {
            if (tick >= game->next_drop_time) {
                e.type = EVT_TIMER;
                return e;
            }
            
            poll_counter++;
            if (poll_counter >= DISPLAY_POLL_INTERVAL) {
                display(game); 
                poll_counter = 0;
                
                if (game->state == GS_ANIMATING) {
                    e.type = EVT_NONE; 
                    return e; 
                }
            }
            
            skipmt();
        }
    }
}

/* -------------------------------------------------------------------
 * 各種ロジック関数
 * ------------------------------------------------------------------- */
int isHit(TetrisGame *game, int _minoX, int _minoY, int _minoType, int _minoAngle) {
    int i, j;
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (minoShapes[_minoType][_minoAngle][i][j]) {
                int fy = _minoY + i;
                int fx = _minoX + j;
                if (fy < 0 || fy >= FIELD_HEIGHT || fx < 0 || fx >= FIELD_WIDTH) return 1;
                if (game->field[fy][fx]) return 1;
            }
        }
    }
    return 0;
}

void fillBag(TetrisGame *game) {
    int i, j, temp;
    for (i = 0; i < 7; i++) game->bag[i] = i;
    /* tickを使ったシャッフル (起動直後の固定化対策済み) */
    for (i = 6; i > 0; i--) {
        j = (tick + rand()) % (i + 1); 
        temp = game->bag[i]; game->bag[i] = game->bag[j]; game->bag[j] = temp;
    }
    game->bag_index = 0;
}

void resetMino(TetrisGame *game) {
    game->minoX = 5;
    game->minoY = 0;
    if (game->bag_index >= 7) fillBag(game);
    game->minoType = game->bag[game->bag_index];
    game->bag_index++;
    game->minoAngle = (tick + rand()) % MINO_ANGLE_MAX;
}

int processGarbage(TetrisGame *game) {
    int lines = game->pending_garbage;
    if (lines <= 0) return 0;
    if (lines > 4) lines = 4;
    game->pending_garbage -= lines;

    int i, j, k;
    for (k = 0; k < lines; k++) {
        for (j = 1; j < FIELD_WIDTH - 1; j++) {
            if (game->field[k][j] != 0) return 1; 
        }
    }
    for (i = 0; i < FIELD_HEIGHT - 1 - lines; i++) {
        memcpy(game->field[i], game->field[i + lines], FIELD_WIDTH);
    }
    for (i = FIELD_HEIGHT - 1 - lines; i < FIELD_HEIGHT - 1; i++) {
        game->field[i][0] = 1;
        game->field[i][FIELD_WIDTH - 1] = 1;
        for (j = 1; j < FIELD_WIDTH - 1; j++) {
            game->field[i][j] = 2 + MINO_TYPE_GARBAGE; 
        }
        int hole = 1 + (tick + rand() + i) % (FIELD_WIDTH - 2);
        game->field[i][hole] = 0;
    }
    return 0; 
}

/* -------------------------------------------------------------------
 * 同期処理関数 (修正版)
 * ------------------------------------------------------------------- */

/* ★修正: ゲーム開始前の同期と乱数初期化 */
void wait_start(TetrisGame *game) {
    int opponent_id = (game->port_id == 0) ? 1 : 0;

    fprintf(game->fp_out, ESC_CLS ESC_HOME);
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "   TETRIS: 2-PLAYER BATTLE  \n");
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "\nPress Any Key to Start...\n");
    fflush(game->fp_out);

    /* 1. キー入力待ち (人間が押すまで待機) */
    while (1) {
        int c = inbyte(game->port_id);
        if (c != -1) break; 
        skipmt();
    }

    /* 2. 乱数初期化: 押された瞬間の tick をシードにする */
    /* これにより、起動直後でも毎回異なるミノ順序になる */
    srand((unsigned int)tick);

    /* 3. 開始同期 */
    game->sync_generation++;
    fprintf(game->fp_out, ESC_CLR_LINE);
    fprintf(game->fp_out, "\rWaiting for opponent...   \n");
    fflush(game->fp_out);

    while (1) {
        if (all_games[opponent_id] != NULL) {
            if (all_games[opponent_id]->sync_generation == game->sync_generation) break;
        } else {
            break; /* 相手がいなければ待たない */
        }
        skipmt();
    }
}

void wait_retry(TetrisGame *game) {
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    fprintf(game->fp_out, "\nPress 'R' to Retry...\n");
    fflush(game->fp_out);
    while (1) {
        int c = inbyte(game->port_id);
        if (c == 'r' || c == 'R') break; 
        skipmt();
    }
    /* リトライ時も念のため乱数を混ぜる */
    srand((unsigned int)tick + rand());

    game->sync_generation++;
    fprintf(game->fp_out, ESC_CLR_LINE);
    fprintf(game->fp_out, "\rWaiting for opponent...   \n");
    fflush(game->fp_out);
    while (1) {
        if (all_games[opponent_id] != NULL) {
            if (all_games[opponent_id]->sync_generation == game->sync_generation) break;
        } else break;
        skipmt();
    }
}

void show_gameover_message(TetrisGame *game) {
    fprintf(game->fp_out, ESC_CLS ESC_HOME "%s", COL_BLUE);
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "         GAME OVER          \n");
    fprintf(game->fp_out, "          YOU LOSE          \n");
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "%sFinal Score: %d\n", ESC_RESET, game->score);
    fflush(game->fp_out);
}

void show_victory_message(TetrisGame *game) {
    fprintf(game->fp_out, ESC_CLS ESC_HOME "%s", COL_RED);
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "      CONGRATULATIONS!      \n");
    fprintf(game->fp_out, "          YOU WIN!          \n");
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "%sScore: %d\n", ESC_RESET, game->score);
    fflush(game->fp_out);
}

/* -------------------------------------------------------------------
 * ゲームロジック本体
 * ------------------------------------------------------------------- */
void run_tetris(TetrisGame *game) {
    int i, j;
    
    game->score = 0;
    game->lines_cleared = 0;
    game->pending_garbage = 0;
    game->is_gameover = 0;
    game->state = GS_PLAYING; 
    game->lines_to_clear = 0;
    
    game->seq_state = 0;
    game->opponent_was_connected = 0;
    
    memset(game->prevBuffer, -1, sizeof(game->prevBuffer));
    memset(game->prevOpponentBuffer, -1, sizeof(game->prevOpponentBuffer));
    
    game->bag_index = 7;

    fprintf(game->fp_out, ESC_CLS ESC_HIDE_CUR); 
    
    memset(game->field, 0, sizeof(game->field));
    for (i = 0; i < FIELD_HEIGHT; i++) {
        game->field[i][0] = game->field[i][FIELD_WIDTH - 1] = 1;
    }
    for (i = 0; i < FIELD_WIDTH; i++) {
        game->field[FIELD_HEIGHT - 1][i] = 1;
    }

    resetMino(game);
    display(game);
    game->next_drop_time = tick + DROP_INTERVAL;

    while (1) {
        Event e = wait_event(game);

        if (game->state == GS_ANIMATING) {
            if (tick >= game->anim_start_tick + ANIMATION_DURATION) {
                fprintf(game->fp_out, ESC_INVERT_OFF); 
                
                game->lines_cleared += game->lines_to_clear;
                
                int attack = 0;
                if (game->lines_to_clear == 1) attack = 1;
                if (game->lines_to_clear == 2) attack = 1;
                if (game->lines_to_clear == 3) attack = 2;
                if (game->lines_to_clear == 4) attack = 4;
                
                if (attack > 0) {
                    int opponent_id = (game->port_id == 0) ? 1 : 0;
                    if (all_games[opponent_id] != NULL && !all_games[opponent_id]->is_gameover) {
                        all_games[opponent_id]->pending_garbage += attack;
                    }
                }
                
                switch (game->lines_to_clear) {
                    case 1: game->score += 100; break;
                    case 2: game->score += 300; break;
                    case 3: game->score += 500; break;
                    case 4: game->score += 800; break;
                }

                game->state = GS_PLAYING;
                game->next_drop_time = tick + DROP_INTERVAL; 
                
                goto PROCESS_GARBAGE;
            }
            continue; 
        }

        switch (e.type) {
            case EVT_WIN:
                show_victory_message(game);
                wait_retry(game); 
                return; 

            case EVT_QUIT:
                fprintf(game->fp_out, "%sQuit.\n", ESC_SHOW_CUR);
                wait_retry(game);
                return;

            case EVT_KEY_INPUT:
                switch (e.param) {
                    case 's':
                        if (!isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                            game->minoY++;
                            game->next_drop_time = tick + DROP_INTERVAL;
                        }
                        break;
                    case 'a':
                        if (!isHit(game, game->minoX - 1, game->minoY, game->minoType, game->minoAngle)) game->minoX--;
                        break;
                    case 'd':
                        if (!isHit(game, game->minoX + 1, game->minoY, game->minoType, game->minoAngle)) game->minoX++;
                        break;
                    case ' ':
                    case 'w':
                        if (!isHit(game, game->minoX, game->minoY, game->minoType, (game->minoAngle + 1) % MINO_ANGLE_MAX)) {
                            game->minoAngle = (game->minoAngle + 1) % MINO_ANGLE_MAX;
                        }
                        break;
                }
                display(game);
                break;

            case EVT_TIMER:
                if (isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                    for (i = 0; i < MINO_HEIGHT; i++) {
                        for (j = 0; j < MINO_WIDTH; j++) {
                            if (minoShapes[game->minoType][game->minoAngle][i][j]) {
                                if (game->minoY + i >= 0 && game->minoY + i < FIELD_HEIGHT &&
                                    game->minoX + j >= 0 && game->minoX + j < FIELD_WIDTH) {
                                    game->field[game->minoY + i][game->minoX + j] = 2 + game->minoType;
                                }
                            }
                        }
                    }
                    
                    int lines_this_turn = 0;
                    for (i = 0; i < FIELD_HEIGHT - 1; i++) {
                        int lineFill = 1;
                        for (j = 1; j < FIELD_WIDTH - 1; j++) {
                            if (game->field[i][j] == 0) { lineFill = 0; break; }
                        }
                        if (lineFill) {
                            int k;
                            for (k = i; k > 0; k--) memcpy(game->field[k], game->field[k - 1], FIELD_WIDTH);
                            memset(game->field[0], 0, FIELD_WIDTH);
                            game->field[0][0] = game->field[0][FIELD_WIDTH-1] = 1;
                            lines_this_turn++;
                        }
                    }

                    if (lines_this_turn > 0) {
                        fprintf(game->fp_out, "\a" ESC_INVERT_ON); 
                        fflush(game->fp_out);
                        game->state = GS_ANIMATING;
                        game->anim_start_tick = tick;
                        game->lines_to_clear = lines_this_turn;
                        break; 
                    }

                PROCESS_GARBAGE:
                    if (processGarbage(game)) {
                        game->is_gameover = 1;
                        fprintf(game->fp_out, "\a");
                        show_gameover_message(game);
                        wait_retry(game); 
                        return;
                    }
                    
                    resetMino(game);
                    
                    if (isHit(game, game->minoX, game->minoY, game->minoType, game->minoAngle)) {
                        game->is_gameover = 1;
                        fprintf(game->fp_out, "\a");
                        show_gameover_message(game);
                        wait_retry(game); 
                        return; 
                    }
                    game->next_drop_time = tick + DROP_INTERVAL;

                } else {
                    game->minoY++;
                    game->next_drop_time = tick + DROP_INTERVAL;
                }
                display(game);
                break;
                
            default:
                break;
        }
    }
}

/* -------------------------------------------------------------------
 * タスク群
 * ------------------------------------------------------------------- */
void task1(void) {
    TetrisGame game1;
    game1.port_id = 0;
    game1.fp_out = com0out;
    game1.sync_generation = 0;
    all_games[0] = &game1; 
    
    /* 修正: ゲーム開始前の同期と乱数初期化 */
    wait_start(&game1);

    while(1) {
        run_tetris(&game1);
    }
}

void task2(void) {
    TetrisGame game2;
    game2.port_id = 1;
    game2.fp_out = com1out;
    game2.sync_generation = 0;
    all_games[1] = &game2;
    
    /* 修正: ゲーム開始前の同期と乱数初期化 */
    wait_start(&game2);

    while(1) {
        run_tetris(&game2);
    }
}

/* -------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------- */
int main(void) {
    init_kernel();
    com0in  = fdopen(0, "r");
    com0out = fdopen(1, "w");
    com1in  = fdopen(4, "r");
    com1out = fdopen(4, "w");

    set_task(task1);
    set_task(task2);

    begin_sch();
    return 0;
}