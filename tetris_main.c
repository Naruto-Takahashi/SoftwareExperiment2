/* ===================================================================
 * tetris_main.c
 * テーマ3対応: イベント駆動型マルチタスク
 * 修正版: 
 * 1. 描画最適化 (行単位インターリーブ) -> 左右同時描画の実現
 * 2. ノンブロッキングアニメーション -> ライン消去時のフリーズ解消
 * 3. 通信量削減 -> 動作の軽量化
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

/* 画面更新の間引き (重さ対策: ループ何回に1回描画チェックするか) */
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
    GS_ANIMATING,   /* ライン消去演出中 (操作不能・ノンブロッキング) */
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
    
    /* 盤面データ */
    char field[FIELD_HEIGHT][FIELD_WIDTH];
    
    /* 描画用バッファ */
    char displayBuffer[FIELD_HEIGHT][FIELD_WIDTH];
    char prevBuffer[FIELD_HEIGHT][FIELD_WIDTH];
    
    /* 相手画面の差分描画用バッファ */
    char prevOpponentBuffer[FIELD_HEIGHT][FIELD_WIDTH]; 
    int opponent_was_connected;

    /* 現在のゲーム状態 (ステートマシン用) */
    GameState state;
    unsigned long anim_start_tick; /* アニメーション開始時刻 */
    int lines_to_clear;            /* 消去待ちのライン数 */

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
    
    /* 相手から送られたお邪魔段数 (volatile: 相手タスクから書き込まれる) */
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
 * 関数プロトタイプ
 * ------------------------------------------------------------------- */
int isHit(TetrisGame *game, int _minoX, int _minoY, int _minoType, int _minoAngle);
void resetMino(TetrisGame *game);
int processGarbage(TetrisGame *game);
void show_gameover_message(TetrisGame *game);
void wait_retry(TetrisGame *game);

/* -------------------------------------------------------------------
 * 描画関連関数
 * ------------------------------------------------------------------- */

/* 1セルの描画文字列を出力する（カーソル移動なし） */
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

/*
 * display: 画面描画関数 (最適化版)
 * 行ごとに、自分と相手の更新分をまとめて処理することで、
 * カーソル移動(エスケープシーケンス)のオーバーヘッドを削減し、
 * 左右の描画タイミングのズレを解消する。
 */
void display(TetrisGame *game) {
    int i, j;
    int changes = 0;
    
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    TetrisGame *opponent = all_games[opponent_id];

    /* 相手接続時の初期化 */
    int opponent_connected = (opponent != NULL);
    if (opponent_connected && !game->opponent_was_connected) {
        memset(game->prevOpponentBuffer, -1, sizeof(game->prevOpponentBuffer));
    }
    game->opponent_was_connected = opponent_connected;

    /* 自分のバッファ更新 */
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

    /* ヘッダー情報（ここは毎回更新してもデータ量が少ないのでOK） */
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

    /* --- 盤面描画（行単位でインターリーブ） --- */
    int base_y = 3;

    for (i = 0; i < FIELD_HEIGHT; i++) {
        /* この行で、自分のフィールドに変化があるか？ */
        for (j = 0; j < FIELD_WIDTH; j++) {
            char myVal = game->displayBuffer[i][j];
            if (myVal != game->prevBuffer[i][j]) {
                /* カーソル移動して描画 */
                fprintf(game->fp_out, "\x1b[%d;%dH", base_y + i, j * 2 + 1);
                print_cell_content(game->fp_out, myVal);
                game->prevBuffer[i][j] = myVal;
                changes++;
            }
        }

        /* 同じ行で、相手のフィールドに変化があるか？ */
        if (opponent != NULL) {
            for (j = 0; j < FIELD_WIDTH; j++) {
                char oppVal = opponent->displayBuffer[i][j];
                if (oppVal != game->prevOpponentBuffer[i][j]) {
                    /* カーソル移動（右側のオフセット位置へ） */
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
 * イベント処理関数
 * ------------------------------------------------------------------- */
Event wait_event(TetrisGame *game) {
    Event e;
    e.type = EVT_NONE;
    int c;
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    static int poll_counter = 0;

    while (1) {
        /* 勝利判定 */
        if (all_games[opponent_id] != NULL && all_games[opponent_id]->is_gameover) {
            e.type = EVT_WIN;
            return e;
        }

        /* アニメーション中は入力やタイマーを無視して、アニメ終了判定のみ行う */
        /* -> 呼び出し元で処理するため、ここでは通常通り返す */

        /* 入力チェック */
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
            /* 時間経過チェック */
            if (tick >= game->next_drop_time) {
                e.type = EVT_TIMER;
                return e;
            }
            
            /* 画面更新 (間引き処理) */
            poll_counter++;
            if (poll_counter >= DISPLAY_POLL_INTERVAL) {
                display(game); /* 相手画面の更新もここで行われる */
                poll_counter = 0;
                
                /* アニメーション中ならここですぐリターンせず、状態チェックさせる */
                if (game->state == GS_ANIMATING) {
                    /* 何もイベントがなくても、アニメ終了判定のために定期的に戻る必要がある */
                    e.type = EVT_NONE; 
                    return e; 
                }
            }
            
            skipmt();
        }
    }
}

/* -------------------------------------------------------------------
 * ゲームロジック (ステートマシン導入)
 * ------------------------------------------------------------------- */
void run_tetris(TetrisGame *game) {
    int i, j;
    
    /* 初期化 */
    game->score = 0;
    game->lines_cleared = 0;
    game->pending_garbage = 0;
    game->is_gameover = 0;
    game->state = GS_PLAYING; /* 初期状態 */
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

    /* メインループ */
    while (1) {
        Event e = wait_event(game);

        /* --- 1. アニメーション中の処理 --- */
        if (game->state == GS_ANIMATING) {
            /* アニメーション期間が終了したかチェック */
            if (tick >= game->anim_start_tick + ANIMATION_DURATION) {
                /* アニメ終了処理 */
                fprintf(game->fp_out, ESC_INVERT_OFF); /* 反転解除 */
                
                /* ライン消去と攻撃処理の実行 */
                game->lines_cleared += game->lines_to_clear;
                
                int attack = 0;
                /* テスト用: 1列でも攻撃 (適宜変更可) */
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

                /* 状態をプレイ中に戻す */
                game->state = GS_PLAYING;
                game->next_drop_time = tick + DROP_INTERVAL; /* 落下タイマー再開 */
                
                /* お邪魔処理へ移行 */
                goto PROCESS_GARBAGE;
            }
            continue; /* アニメ中は入力を無視 */
        }

        /* --- 2. 通常プレイ中の処理 --- */
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
                /* 自然落下処理 */
                if (isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                    /* 固定処理 */
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
                    
                    /* ライン消去チェック */
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
                        /* アニメーション開始 (ノンブロッキング) */
                        fprintf(game->fp_out, "\a" ESC_INVERT_ON); /* 音+反転 */
                        fflush(game->fp_out);
                        
                        game->state = GS_ANIMATING;
                        game->anim_start_tick = tick;
                        game->lines_to_clear = lines_this_turn;
                        
                        /* 次のループへ (アニメ終了を待つ) */
                        break; 
                    }

                PROCESS_GARBAGE:
                    /* お邪魔ブロックのせり上がり */
                    if (processGarbage(game)) {
                        game->is_gameover = 1;
                        fprintf(game->fp_out, "\a");
                        show_gameover_message(game);
                        wait_retry(game); 
                        return;
                    }
                    
                    /* 次のミノ */
                    resetMino(game);
                    
                    /* 窒息判定 */
                    if (isHit(game, game->minoX, game->minoY, game->minoType, game->minoAngle)) {
                        game->is_gameover = 1;
                        fprintf(game->fp_out, "\a");
                        show_gameover_message(game);
                        wait_retry(game); 
                        return; 
                    }
                    
                    /* 更新: next_drop_time はリセット */
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
 * その他関数
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

void wait_retry(TetrisGame *game) {
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    fprintf(game->fp_out, "\nPress 'R' to Retry...\n");
    fflush(game->fp_out);
    while (1) {
        int c = inbyte(game->port_id);
        if (c == 'r' || c == 'R') break; 
        skipmt();
    }
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
 * タスク群
 * ------------------------------------------------------------------- */
void task1(void) {
    TetrisGame game1;
    game1.port_id = 0;
    game1.fp_out = com0out;
    game1.sync_generation = 0;
    all_games[0] = &game1; 
    while(1) run_tetris(&game1);
}

void task2(void) {
    TetrisGame game2;
    game2.port_id = 1;
    game2.fp_out = com1out;
    game2.sync_generation = 0;
    all_games[1] = &game2;
    while(1) run_tetris(&game2);
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