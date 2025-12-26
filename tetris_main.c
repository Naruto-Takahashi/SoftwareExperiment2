/* ===========================================================================
 * ファイル名 : tetris_main.c
 * テーマ     : テーマ3 応用
 * 作成者     : 高橋 成翔
 * 作成日     : 2025/12/26
 *
 * [概要]
 * MC68VZ328用マルチタスクカーネル上で動作する2人対戦型テトリス。
 * 2つのシリアルポートを利用して、対戦相手と画面情報を共有しながらプレイする。
 *
 * [タスク構成]
 * 1. task1 (User Task 1): Player 1 (Port 0) のゲームロジック
 * 2. task2 (User Task 2): Player 2 (Port 1) のゲームロジック
 * 3. task_turbo_monitor : 時間経過を監視し、難易度上昇とLED演出を行う管理タスク
 *
 * [主な機能]
 * - 共有メモリとセマフォを用いたお邪魔ブロック攻撃
 * - 経過時間に応じた落下速度・スコア倍率の上昇 (ターボ機能)
 * - ダブルバッファリングによる差分描画 (通信量削減)
 * =========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mtk_c.h"

/* ***************************************************************************
 * 1. ハードウェア定義 & 定数マクロ
 * *************************************************************************** */

/* --- メモリマップ定義 --- */
#define IOBASE  0x00D00000

/* --- LEDアドレス定義 --- */
/* 実機仕様に基づき，IOBASEからのオフセットで各LEDのアドレスを配列化 */
unsigned char * const leds[8] = {
    (unsigned char *)(IOBASE + 0x00000039), /* LED0 */
    (unsigned char *)(IOBASE + 0x0000003b), /* LED1 */
    (unsigned char *)(IOBASE + 0x0000003d), /* LED2 */
    (unsigned char *)(IOBASE + 0x0000003f), /* LED3 */
    (unsigned char *)(IOBASE + 0x00000029), /* LED4 */
    (unsigned char *)(IOBASE + 0x0000002b), /* LED5 */
    (unsigned char *)(IOBASE + 0x0000002d), /* LED6 */
    (unsigned char *)(IOBASE + 0x0000002f)  /* LED7 */
};

/* --- 外部依存定義 (カーネル/ライブラリ) --- */
extern FILE *com0in, *com0out;
extern FILE *com1in, *com1out;
extern void init_kernel(void);
extern void set_task(void (*func)());
extern void begin_sch(void);
extern int inbyte(int ch);
extern void skipmt(void);
extern void P(int sem_id);
extern void V(int sem_id);
extern volatile unsigned long tick;
extern SEMAPHORE_TYPE semaphore[NUMSEMAPHORE];

/* ***************************************************************************
 * 2. システム状態管理・調整パラメータ
 * *************************************************************************** */

/* --- システムのフェーズ定義 --- */
/* ゲーム全体の進行状態を管理し、ターボタスクの動作制御に使用する */
enum {
    PHASE_IDLE,      /* 待機中・リセット中 (時間停止・LED消灯) */
    PHASE_COUNTDOWN, /* カウントダウン中 (時間停止・ゲージリセット) */
    PHASE_PLAYING,   /* プレイ中 (時間進行・LEDアニメーション) */
    PHASE_RESULT     /* ゲーム終了・結果表示 (時間停止・LED状態維持) */
};

/* 共有状態フラグ (初期値: IDLE) */
/* Player 1 のタスクが代表してこのフラグを書き換える */
volatile int g_system_phase = PHASE_IDLE; 

/* --- ターボ機能・実機調整用パラメータ (定数) --- */
/* 以下の定数は実機でのゲームバランス調整に使用する */
#define TURBO_MAX_LEVEL_TIME_SEC 180 /* MAXレベル(Lv8)到達までの所要時間 (秒) */
#define TURBO_BASE_INTERVAL      600 /* レベル0時の基本落下速度 (tick) */
#define TURBO_TICKS_PER_SEC      100 /* 1秒あたりのtick数 (カーネル仕様に依存) */
#define TURBO_UPDATE_PERIOD      1   /* ターボ監視タスクの更新周期 (小さいほど高頻度) */
#define TURBO_BLINK_CYCLE        1   /* MAX時の点滅速度調整 (N回に1回反転) */

/* --- ターボシステム用共有変数 (計算結果保持用) --- */
/* task_turbo_monitor が計算し、各ゲームタスクが読み取る */
volatile unsigned long g_current_drop_interval = TURBO_BASE_INTERVAL;
volatile int g_score_multiplier = 1;

/* セマフォID定義 */
#define SEM_GARBAGE_LOCK 0  /* お邪魔ブロック変数の排他制御用 */

/* ***************************************************************************
 * 3. ゲーム設定 & エスケープシーケンス
 * *************************************************************************** */

/* --- ゲームパラメータ --- */
#define FIELD_WIDTH  12       /* 壁を含むフィールド幅 */
#define FIELD_HEIGHT 22       /* 壁を含むフィールド高さ */
#define MINO_WIDTH   4        /* ミノのグリッドサイズ */
#define MINO_HEIGHT  4        /* ミノのグリッドサイズ */
#define OPPONENT_OFFSET_X 40  /* 相手画面を表示するX座標のオフセット */
#define ANIMATION_DURATION 3  /* ライン消去アニメーションの長さ (tick) */
#define COUNTDOWN_DELAY 10000 /* カウントダウンの待機時間 (実機調整値) */
#define DISPLAY_POLL_INTERVAL 50 /* 入力待ち時の画面更新頻度 */

/* フィールドのセル値 */
#define CELL_EMPTY  0
#define CELL_WALL   1
#define CELL_GHOST  10

/* --- エスケープシーケンス (VT100互換) --- */
#define ESC_CLS        "\x1b[2J"    /* 画面クリア */
#define ESC_HOME       "\x1b[H"     /* カーソルホーム */
#define ESC_RESET      "\x1b[0m"    /* 属性リセット */
#define ESC_HIDE_CUR   "\x1b[?25l"  /* カーソル非表示 */
#define ESC_SHOW_CUR   "\x1b[?25h"  /* カーソル表示 */
#define ESC_CLR_LINE   "\x1b[K"     /* 行末まで消去 */
#define ESC_INVERT_ON  "\x1b[?5h"   /* 画面反転 (フラッシュ演出用) */
#define ESC_INVERT_OFF "\x1b[?5l"   /* 画面反転解除 */

/* --- カラー定義 (24bitカラーなど) --- */
#define COL_CYAN     "\x1b[38;2;0;255;255m"
#define COL_YELLOW   "\x1b[38;2;255;255;0m"
#define COL_PURPLE   "\x1b[38;2;160;32;240m"
#define COL_BLUE     "\x1b[38;2;0;0;255m"
#define COL_ORANGE   "\x1b[38;2;255;165;0m"
#define COL_GREEN    "\x1b[38;2;0;255;0m"
#define COL_RED      "\x1b[38;2;255;0;0m"
#define COL_WHITE    "\x1b[38;2;255;255;255m"
#define COL_GRAY     "\x1b[38;2;128;128;128m"
#define COL_WALL     COL_WHITE
#define BG_BLACK     "\x1b[40m"

/* ***************************************************************************
 * 4. 構造体・データ型定義
 * *************************************************************************** */

/* 状態・イベント定義 */
typedef enum { 
    GS_PLAYING,   /* 通常プレイ中 */
    GS_ANIMATING, /* ライン消去アニメーション中 */
    GS_GAMEOVER   /* ゲームオーバー */
} GameState;

typedef enum { 
    EVT_NONE,      /* イベントなし */
    EVT_KEY_INPUT, /* キー入力あり */
    EVT_TIMER,     /* 落下タイマ発火 */
    EVT_WIN,       /* 勝利確定 */
    EVT_QUIT       /* 強制終了 */
} EventType;

/* イベント構造体 */
typedef struct {
    EventType type;
    int param;     /* キーコード等のパラメータ */
} Event;

/* テトリスゲーム管理構造体 */
typedef struct {
    /* 通信・IO関連 */
    int port_id;   /* 0:UART1, 1:UART2 */
    FILE *fp_out;  /* 出力ストリーム */
    
    /* 画面バッファ (ダブルバッファリング用) */
    char field[FIELD_HEIGHT][FIELD_WIDTH];              /* 現在のフィールド状態 */
    char displayBuffer[FIELD_HEIGHT][FIELD_WIDTH];      /* 描画用バッファ */
    char prevBuffer[FIELD_HEIGHT][FIELD_WIDTH];         /* 前回描画した内容 (自分) */
    char prevOpponentBuffer[FIELD_HEIGHT][FIELD_WIDTH]; /* 前回描画した内容 (相手) */
    int opponent_was_connected;                         /* 相手接続フラグ */
    
    /* 進行状態 */
    GameState state;
    unsigned long anim_start_tick;
    int lines_to_clear;
    
    /* ミノ制御 */
    int minoType, minoAngle, minoX, minoY;
    int nextMinoType, prevNextMinoType;
    int bag[7], bag_index; /* 7種1巡生成用バッグ */
    
    /* タイミング・入力制御 */
    unsigned long next_drop_time;
    int seq_state; /* エスケープシーケンス解析用ステート */
    
    /* スコア・統計・共有情報 (他タスクから参照される変数はvolatile) */
    int score;
    int lines_cleared;
    volatile int pending_garbage; /* 受け取ったお邪魔ライン数 */
    volatile int is_gameover;     /* ゲームオーバー状態 */
    volatile int sync_generation; /* 開始同期用世代カウンタ */
} TetrisGame;

/* 相手タスク参照用ポインタ配列 */
TetrisGame *all_games[2] = {NULL, NULL};

/* ミノ定義 */
enum { MINO_TYPE_I, MINO_TYPE_O, MINO_TYPE_S, MINO_TYPE_Z, MINO_TYPE_J, MINO_TYPE_L, MINO_TYPE_T, MINO_TYPE_GARBAGE, MINO_TYPE_MAX };
enum { MINO_ANGLE_0, MINO_ANGLE_90, MINO_ANGLE_180, MINO_ANGLE_270, MINO_ANGLE_MAX };
const char* minoColors[MINO_TYPE_MAX] = { COL_CYAN, COL_YELLOW, COL_GREEN, COL_RED, COL_BLUE, COL_ORANGE, COL_PURPLE, COL_GRAY };

/* ミノ形状データ [種類][角度][y][x] */
char minoShapes[MINO_TYPE_MAX][MINO_ANGLE_MAX][MINO_HEIGHT][MINO_WIDTH] = {
    /* I */ { {{0,1,0,0},{0,1,0,0},{0,1,0,0},{0,1,0,0}}, {{0,0,0,0},{0,0,0,0},{1,1,1,1},{0,0,0,0}}, {{0,0,1,0},{0,0,1,0},{0,0,1,0},{0,0,1,0}}, {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}} },
    /* O */ { {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}} },
    /* S */ { {{0,0,0,0},{0,1,1,0},{1,1,0,0},{0,0,0,0}}, {{0,1,0,0},{0,1,1,0},{0,0,1,0},{0,0,0,0}}, {{0,0,0,0},{0,0,1,1},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,0,0},{0,1,1,0},{0,0,1,0}} },
    /* Z */ { {{0,0,0,0},{1,1,0,0},{0,1,1,0},{0,0,0,0}}, {{0,0,1,0},{0,1,1,0},{0,1,0,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,0,1,1},{0,0,0,0}}, {{0,0,0,0},{0,0,1,0},{0,1,1,0},{0,1,0,0}} },
    /* J */ { {{0,0,1,0},{0,0,1,0},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,0,0},{0,1,1,1},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,1,0,0},{0,1,0,0}}, {{0,0,0,0},{1,1,1,0},{0,0,1,0},{0,0,0,0}} },
    /* L */ { {{0,1,0,0},{0,1,0,0},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,1},{0,1,0,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,0,1,0},{0,0,1,0}}, {{0,0,0,0},{0,0,1,0},{1,1,1,0},{0,0,0,0}} },
    /* T */ { {{0,0,0,0},{1,1,1,0},{0,1,0,0},{0,0,0,0}}, {{0,0,1,0},{0,1,1,0},{0,0,1,0},{0,0,0,0}}, {{0,0,0,0},{0,0,1,0},{0,1,1,1},{0,0,0,0}}, {{0,0,0,0},{0,1,0,0},{0,1,1,0},{0,1,0,0}} },
    /* G */ { {{0}},{{0}},{{0}},{{0}} }
};

/* ***************************************************************************
 * 5. 関数プロトタイプ宣言
 * *************************************************************************** */
int  isHit(TetrisGame *game, int _minoX, int _minoY, int _minoType, int _minoAngle);
void print_cell_content(FILE *fp, char cellVal);
void display(TetrisGame *game);
void perform_countdown(TetrisGame *game);
void wait_start(TetrisGame *game);
void wait_retry(TetrisGame *game);
void show_gameover_message(TetrisGame *game);
void show_victory_message(TetrisGame *game);
void run_tetris(TetrisGame *game);
void task_turbo_monitor(void);

/* ***************************************************************************
 * 6. 描画・表示関連関数
 * *************************************************************************** */

/* ---------------------------------------------------------------------------
 * 関数名 : print_cell_content
 * 概要   : セル1つ分の描画内容を出力ストリームに書き込む
 * 引数   : fp      - 出力先ファイルポインタ
 * cellVal - セルの値 (0:空, 1:壁, 2-9:ミノ, 10:ゴースト)
 * --------------------------------------------------------------------------- */
void print_cell_content(FILE *fp, char cellVal) {
    if (cellVal == CELL_EMPTY) {
        fprintf(fp, "%s・%s", BG_BLACK, ESC_RESET);
    } else if (cellVal == CELL_WALL) {
        fprintf(fp, "%s%s■%s", BG_BLACK, COL_WALL, ESC_RESET);
    } else if (cellVal == CELL_GHOST) {
        fprintf(fp, "%s%s□%s", BG_BLACK, COL_GRAY, ESC_RESET);
    } else if (cellVal >= 2 && cellVal <= 9) {
        fprintf(fp, "%s%s■%s", BG_BLACK, minoColors[cellVal - 2], ESC_RESET);
    } else {
        fprintf(fp, "??");
    }
}

/* ---------------------------------------------------------------------------
 * 関数名 : display
 * 概要   : 画面全体の描画処理 (ダブルバッファリング差分更新)
 * 引数   : game - 描画対象のゲームインスタンス
 * 詳細   : 
 * 通信量を削減するため、前回の描画内容(prevBuffer)と比較し、
 * 変更があったセルのみカーソル移動して再描画を行う。
 * 対戦相手が接続されている場合は、右側に相手のフィールドも描画する。
 * --------------------------------------------------------------------------- */
void display(TetrisGame *game) {
    int i, j;
    int changes = 0;
    
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    TetrisGame *opponent = all_games[opponent_id];

    /* 相手接続検知時のバッファリセット (初期化漏れ防止) */
    int opponent_connected = (opponent != NULL);
    if (opponent_connected && !game->opponent_was_connected) {
        memset(game->prevOpponentBuffer, -1, sizeof(game->prevOpponentBuffer));
    }
    game->opponent_was_connected = opponent_connected;

    /* [Step 1] 描画バッファ構築 (現在のフィールド + 操作中ミノ + ゴースト) */
    memcpy(game->displayBuffer, game->field, sizeof(game->field));

    /* ゴースト描画 (落下地点の予測) */
    if (game->minoType != MINO_TYPE_GARBAGE) {
        int ghostY = game->minoY;
        /* 接地するまでY座標を下げる */
        while (!isHit(game, game->minoX, ghostY + 1, game->minoType, game->minoAngle)) {
            ghostY++;
        }
        /* ゴーストをバッファに書き込み */
        for (i = 0; i < MINO_HEIGHT; i++) {
            for (j = 0; j < MINO_WIDTH; j++) {
                if (minoShapes[game->minoType][game->minoAngle][i][j]) {
                    if (ghostY + i >= 0 && ghostY + i < FIELD_HEIGHT &&
                        game->minoX + j >= 0 && game->minoX + j < FIELD_WIDTH) {
                        if (game->displayBuffer[ghostY + i][game->minoX + j] == CELL_EMPTY) {
                            game->displayBuffer[ghostY + i][game->minoX + j] = CELL_GHOST;
                        }
                    }
                }
            }
        }
    }

    /* 操作中ミノの描画 */
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

    /* [Step 2] ヘッダ情報描画 (スコア等) */
    fprintf(game->fp_out, "\x1b[1;1H"); 
    fprintf(game->fp_out, "[YOU] SC:%-5d x%d ATK:%d", 
            game->score, g_score_multiplier, game->pending_garbage);
    
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

    /* [Step 3] フィールドの差分描画 */
    int base_y = 3;
    for (i = 0; i < FIELD_HEIGHT; i++) {
        /* 自分自身のフィールド */
        for (j = 0; j < FIELD_WIDTH; j++) {
            char myVal = game->displayBuffer[i][j];
            if (myVal != game->prevBuffer[i][j]) {
                fprintf(game->fp_out, "\x1b[%d;%dH", base_y + i, j * 2 + 1);
                print_cell_content(game->fp_out, myVal);
                game->prevBuffer[i][j] = myVal;
                changes++;
            }
        }
        /* 対戦相手のフィールド (接続時のみ) */
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
    /* 変更があった場合のみバッファをフラッシュ */
    if (changes > 0) fflush(game->fp_out);
}

/* ---------------------------------------------------------------------------
 * 関数名 : perform_countdown
 * 概要   : ゲーム開始前のカウントダウン演出 (3, 2, 1, GO!)
 * 引数   : game - ゲームインスタンス
 * --------------------------------------------------------------------------- */
void perform_countdown(TetrisGame *game) {
    const char *messages[] = {" 3 ", " 2 ", " 1 ", "GO!"};
    int i;
    int base_y = 3 + (FIELD_HEIGHT / 2) - 1;
    int base_x = 10; 

    for (i = 0; i < 4; i++) {
        fprintf(game->fp_out, "\x1b[%d;%dH%s%s   %s   %s", 
                base_y, base_x - 1, BG_BLACK, COL_YELLOW, messages[i], ESC_RESET);
        fflush(game->fp_out);
        if (i == 3) break;
        
        /* 待機 (定数 COUNTDOWN_DELAY 使用) */
        unsigned long target_tick = tick + COUNTDOWN_DELAY;
        while (tick < target_tick) skipmt();
    }
    /* 描画クリアのために前回のバッファ内容を無効化 */
    memset(game->prevBuffer, -1, sizeof(game->prevBuffer));
}

/* ***************************************************************************
 * 7. イベント制御
 * *************************************************************************** */

/* ---------------------------------------------------------------------------
 * 関数名 : wait_event
 * 概要   : イベント待機ループ (ノンブロッキング入力)
 * 戻り値 : 発生したイベント構造体
 * 詳細   : 
 * キー入力、タイマ発火、勝利判定などを監視する。
 * 入力がない間は skipmt() を呼び出し、CPU権を他タスクに譲る。
 * --------------------------------------------------------------------------- */
Event wait_event(TetrisGame *game) {
    Event e;
    e.type = EVT_NONE;
    int c;
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    static int poll_counter = 0;

    while (1) {
        /* 1. 勝利判定 (相手がゲームオーバーになったか) */
        if (all_games[opponent_id] != NULL && all_games[opponent_id]->is_gameover) {
            e.type = EVT_WIN; return e;
        }

        /* 2. 入力チェック (ノンブロッキング) */
        c = inbyte(game->port_id);
        if (c != -1) {
            /* エスケープシーケンス解析 (矢印キー対応) */
            if (game->seq_state == 0) {
                if (c == 0x1b) game->seq_state = 1;      /* ESC受信 */
                else if (c == 'q') { e.type = EVT_QUIT; return e; }
                else { e.type = EVT_KEY_INPUT; e.param = c; return e; }
            } 
            else if (game->seq_state == 1) {
                if (c == '[') game->seq_state = 2; else game->seq_state = 0;
            } 
            else if (game->seq_state == 2) {
                game->seq_state = 0;
                /* 矢印キーコードの変換 */
                switch (c) {
                    case 'A': e.param = 'w'; break; /* 上 */
                    case 'B': e.param = 's'; break; /* 下 */
                    case 'C': e.param = 'd'; break; /* 右 */
                    case 'D': e.param = 'a'; break; /* 左 */
                    default:  e.param = 0;   break;
                }
                if (e.param != 0) { e.type = EVT_KEY_INPUT; return e; }
            }
        } 
        else {
            /* 3. タイマ判定 (自然落下) */
            if (tick >= game->next_drop_time) {
                e.type = EVT_TIMER; return e;
            }
            /* 4. アイドル時の定期描画更新 (相手の動きを反映するため) */
            poll_counter++;
            if (poll_counter >= DISPLAY_POLL_INTERVAL) {
                display(game); poll_counter = 0;
                /* アニメーション中はイベントとして返さず継続 */
                if (game->state == GS_ANIMATING) { e.type = EVT_NONE; return e; }
            }
            /* CPU権を他のタスクへ譲渡 */
            skipmt();
        }
    }
}

/* ***************************************************************************
 * 8. ゲームロジック
 * *************************************************************************** */

/* ---------------------------------------------------------------------------
 * 関数名 : isHit
 * 概要   : ミノの衝突判定
 * 戻り値 : 1=衝突あり, 0=なし
 * --------------------------------------------------------------------------- */
int isHit(TetrisGame *game, int _minoX, int _minoY, int _minoType, int _minoAngle) {
    int i, j;
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (minoShapes[_minoType][_minoAngle][i][j]) {
                int fy = _minoY + i;
                int fx = _minoX + j;
                /* フィールド外判定 */
                if (fy < 0 || fy >= FIELD_HEIGHT || fx < 0 || fx >= FIELD_WIDTH) return 1;
                /* 既存ブロックとの衝突判定 */
                if (game->field[fy][fx]) return 1;
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * 関数名 : fillBag
 * 概要   : ミノ生成用バッグの補充 (7種1巡の法則)
 * --------------------------------------------------------------------------- */
void fillBag(TetrisGame *game) {
    int i, j, temp;
    /* 0-6のミノIDをセット */
    for (i = 0; i < 7; i++) game->bag[i] = i;
    /* シャッフル */
    for (i = 6; i > 0; i--) {
        j = (tick + rand()) % (i + 1); 
        temp = game->bag[i]; game->bag[i] = game->bag[j]; game->bag[j] = temp;
    }
    game->bag_index = 0;
}

/* ---------------------------------------------------------------------------
 * 関数名 : resetMino
 * 概要   : 新しいミノの出現処理
 * --------------------------------------------------------------------------- */
void resetMino(TetrisGame *game) {
    game->minoX = 5;
    game->minoY = 0;
    game->minoType = game->nextMinoType;
    game->minoAngle = (tick + rand()) % MINO_ANGLE_MAX;
    
    /* バッグが空なら補充 */
    if (game->bag_index >= 7) fillBag(game);
    
    game->nextMinoType = game->bag[game->bag_index];
    game->bag_index++;
}

/* ---------------------------------------------------------------------------
 * 関数名 : processGarbage
 * 概要   : お邪魔ブロックの処理
 * 詳細   : セマフォを用いて共有変数 pending_garbage を排他制御しながら読み書きする。
 * 戻り値 : 1 = 押し出されてゲームオーバー, 0 = 正常
 * --------------------------------------------------------------------------- */
int processGarbage(TetrisGame *game) {
    int lines;
    
    /* --- クリティカルセクション開始 --- */
    P(SEM_GARBAGE_LOCK);
    lines = game->pending_garbage;
    if (lines > 0) {
        if (lines > 4) {
            game->pending_garbage -= 4; lines = 4; /* 一度は4行まで */
        } else {
            game->pending_garbage = 0;
        }
    }
    V(SEM_GARBAGE_LOCK);
    /* --- クリティカルセクション終了 --- */

    if (lines <= 0) return 0;

    int i, j, k;
    /* 最上段にブロックがあるか確認 (ゲームオーバー判定) */
    for (k = 0; k < lines; k++) {
        for (j = 1; j < FIELD_WIDTH - 1; j++) {
            if (game->field[k][j] != 0) return 1; 
        }
    }
    /* フィールド全体を上にシフト */
    for (i = 0; i < FIELD_HEIGHT - 1 - lines; i++) {
        memcpy(game->field[i], game->field[i + lines], FIELD_WIDTH);
    }
    /* 下段にお邪魔ライン生成 (穴あきブロック列) */
    for (i = FIELD_HEIGHT - 1 - lines; i < FIELD_HEIGHT - 1; i++) {
        game->field[i][0] = 1; game->field[i][FIELD_WIDTH - 1] = 1; /* 壁 */
        for (j = 1; j < FIELD_WIDTH - 1; j++) game->field[i][j] = 2 + MINO_TYPE_GARBAGE; 
        /* ランダムに1箇所穴を開ける */
        int hole = 1 + (tick + rand() + i) % (FIELD_WIDTH - 2);
        game->field[i][hole] = 0;
    }
    return 0; 
}

/* ***************************************************************************
 * 9. 画面遷移・同期処理
 * *************************************************************************** */

/* ---------------------------------------------------------------------------
 * 関数名 : wait_start
 * 概要   : ゲーム開始時の同期待機
 * 詳細   : 双方の準備が整うまで待機し、乱数シードを初期化する。
 * --------------------------------------------------------------------------- */
void wait_start(TetrisGame *game) {
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    fprintf(game->fp_out, ESC_CLS ESC_HOME);
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "   TETRIS: 2-PLAYER BATTLE  \n");
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "\nPress Any Key to Start...\n");
    fflush(game->fp_out);

    /* キー入力待ち */
    while (1) { if (inbyte(game->port_id) != -1) break; skipmt(); }
    
    srand((unsigned int)tick); /* 乱数初期化 */
    game->sync_generation++;
    
    fprintf(game->fp_out, ESC_CLR_LINE "\rWaiting for opponent...   \n");
    fflush(game->fp_out);

    /* 相手との同期 (sync_generationの一致を確認) */
    while (1) {
        if (all_games[opponent_id] != NULL) {
            if (all_games[opponent_id]->sync_generation == game->sync_generation) break;
        } else break; /* 相手がいない場合は即開始 */
        skipmt();
    }
}

/* ---------------------------------------------------------------------------
 * 関数名 : wait_retry
 * 概要   : ゲーム終了後のリトライ待機
 * --------------------------------------------------------------------------- */
void wait_retry(TetrisGame *game) {
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    fprintf(game->fp_out, "\nPress 'R' to Retry...\n");
    fflush(game->fp_out);
    
    while (1) {
        int c = inbyte(game->port_id);
        if (c == 'r' || c == 'R') break; 
        skipmt();
    }
    srand((unsigned int)tick + rand());
    game->sync_generation++;
    fprintf(game->fp_out, ESC_CLR_LINE "\rWaiting for opponent...   \n");
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

/* ***************************************************************************
 * 10. メインゲームループ
 * *************************************************************************** */

/* ---------------------------------------------------------------------------
 * 関数名 : run_tetris
 * 概要   : テトリスのメイン実行ループ
 * 詳細   : 
 * ゲームの初期化、メインループ、終了処理を行う。
 * Player 1 の場合、システムフェーズ (g_system_phase) の制御も行う。
 * --------------------------------------------------------------------------- */
void run_tetris(TetrisGame *game) {
    int i;
    
    /* リトライ時の初期フェーズ設定 (Player 1のみが更新) */
    if (game->port_id == 0) g_system_phase = PHASE_IDLE;
    
    /* 変数初期化 */
    game->score = 0; game->lines_cleared = 0; game->pending_garbage = 0;
    game->is_gameover = 0; game->state = GS_PLAYING; 
    game->lines_to_clear = 0; game->seq_state = 0;
    game->opponent_was_connected = 0; game->prevNextMinoType = -1; 
    
    /* バッファ・画面初期化 */
    memset(game->prevBuffer, -1, sizeof(game->prevBuffer));
    memset(game->prevOpponentBuffer, -1, sizeof(game->prevOpponentBuffer));
    fprintf(game->fp_out, ESC_CLS ESC_HIDE_CUR); 
    
    /* フィールド枠作成 */
    memset(game->field, 0, sizeof(game->field));
    for (i = 0; i < FIELD_HEIGHT; i++) game->field[i][0] = game->field[i][FIELD_WIDTH - 1] = 1; 
    for (i = 0; i < FIELD_WIDTH; i++) game->field[FIELD_HEIGHT - 1][i] = 1; 

    /* ミノ生成 */
    fillBag(game); game->nextMinoType = game->bag[game->bag_index++]; resetMino(game); 
    
    display(game);
    
    /* カウントダウン開始合図 */
    if (game->port_id == 0) g_system_phase = PHASE_COUNTDOWN;
    perform_countdown(game);
    display(game);
    
    /* ゲーム開始合図 (ここからゲージ進行開始) */
    if (game->port_id == 0) g_system_phase = PHASE_PLAYING;
    
    game->next_drop_time = tick + g_current_drop_interval;

    /* イベントループ */
    while (1) {
        Event e = wait_event(game);

        /* --- アニメーション処理 --- */
        if (game->state == GS_ANIMATING) {
            /* 一定時間経過したらアニメーション終了 */
            if (tick >= game->anim_start_tick + ANIMATION_DURATION) {
                fprintf(game->fp_out, ESC_INVERT_OFF); 
                game->lines_cleared += game->lines_to_clear;
                
                /* 攻撃力の計算 */
                int attack = 0;
                switch(game->lines_to_clear) {
                    case 2: attack = 1; break;
                    case 3: attack = 2; break;
                    case 4: attack = 4; break;
                }
                
                /* お邪魔ブロックの送信 (排他制御) */
                if (attack > 0) {
                    int opponent_id = (game->port_id == 0) ? 1 : 0;
                    if (all_games[opponent_id] != NULL && !all_games[opponent_id]->is_gameover) {
                        P(SEM_GARBAGE_LOCK);
                        all_games[opponent_id]->pending_garbage += attack;
                        V(SEM_GARBAGE_LOCK);
                    }
                }
                
                /* スコア計算 (ターボ倍率適用) */
                int base_points = 0;
                switch (game->lines_to_clear) {
                    case 1: base_points = 100; break;
                    case 2: base_points = 300; break;
                    case 3: base_points = 500; break;
                    case 4: base_points = 800; break;
                }
                game->score += base_points * g_score_multiplier;

                game->state = GS_PLAYING;
                game->next_drop_time = tick + g_current_drop_interval; 
                
                /* アニメーション明けに即座にブロック生成処理へ */
                goto PROCESS_GARBAGE;
            }
            continue; 
        }

        /* --- 通常イベント処理 --- */
        switch (e.type) {
            case EVT_WIN:
                /* 勝利時もゲーム終了合図 */
                if (game->port_id == 0) g_system_phase = PHASE_RESULT;
                show_victory_message(game); wait_retry(game); return; 
            case EVT_QUIT:
                if (game->port_id == 0) g_system_phase = PHASE_RESULT;
                fprintf(game->fp_out, "%sQuit.\n", ESC_SHOW_CUR); wait_retry(game); return;
            case EVT_KEY_INPUT:
                /* キー操作 (移動・回転) */
                switch (e.param) {
                    case 's': 
                        if (!isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                            game->minoY++; game->next_drop_time = tick + g_current_drop_interval;
                        }
                        break;
                    case 'a': 
                        if (!isHit(game, game->minoX - 1, game->minoY, game->minoType, game->minoAngle)) game->minoX--;
                        break;
                    case 'd': 
                        if (!isHit(game, game->minoX + 1, game->minoY, game->minoType, game->minoAngle)) game->minoX++;
                        break;
                    case ' ': 
                        {
                            int newAngle = (game->minoAngle + 1) % MINO_ANGLE_MAX;
                            if (!isHit(game, game->minoX, game->minoY, game->minoType, newAngle)) game->minoAngle = newAngle;
                            else if (!isHit(game, game->minoX + 1, game->minoY, game->minoType, newAngle)) { game->minoX++; game->minoAngle = newAngle; }
                            else if (!isHit(game, game->minoX - 1, game->minoY, game->minoType, newAngle)) { game->minoX--; game->minoAngle = newAngle; }
                        }
                        break;
                    case 'w': 
                        while (!isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                            game->minoY++; game->score += 2 * g_score_multiplier; 
                        }
                        display(game); goto LOCK_PROCESS; 
                        break;
                }
                display(game);
                break;

            case EVT_TIMER:
                /* 自然落下処理 */
                if (isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                LOCK_PROCESS: 
                    /* 1. フィールドへの固定 */
                    for (i = 0; i < MINO_HEIGHT; i++) {
                        for (int j = 0; j < MINO_WIDTH; j++) {
                            if (minoShapes[game->minoType][game->minoAngle][i][j]) {
                                if (game->minoY + i >= 0 && game->minoY + i < FIELD_HEIGHT &&
                                    game->minoX + j >= 0 && game->minoX + j < FIELD_WIDTH) {
                                    game->field[game->minoY + i][game->minoX + j] = 2 + game->minoType;
                                }
                            }
                        }
                    }
                    /* 2. ライン消去判定 */
                    int lines_this_turn = 0;
                    for (i = 0; i < FIELD_HEIGHT - 1; i++) {
                        int lineFill = 1;
                        for (int j = 1; j < FIELD_WIDTH - 1; j++) if (game->field[i][j] == 0) { lineFill = 0; break; }
                        if (lineFill) {
                            /* ライン削除と詰め処理 */
                            int k;
                            for (k = i; k > 0; k--) memcpy(game->field[k], game->field[k - 1], FIELD_WIDTH);
                            memset(game->field[0], 0, FIELD_WIDTH);
                            game->field[0][0] = game->field[0][FIELD_WIDTH-1] = 1;
                            lines_this_turn++;
                        }
                    }
                    /* 3. 消去があった場合のアニメーション移行 */
                    if (lines_this_turn > 0) {
                        fprintf(game->fp_out, "\a" ESC_INVERT_ON); 
                        fflush(game->fp_out);
                        game->state = GS_ANIMATING;
                        game->anim_start_tick = tick;
                        game->lines_to_clear = lines_this_turn;
                        break; 
                    }
                PROCESS_GARBAGE: 
                    /* 4. お邪魔ブロックのせり上がり処理 */
                    if (processGarbage(game)) {
                        game->is_gameover = 1;
                        if (game->port_id == 0) g_system_phase = PHASE_RESULT;
                        fprintf(game->fp_out, "\a"); show_gameover_message(game); wait_retry(game); return;
                    }
                    /* 5. 次のミノのリセットと窒息判定 */
                    resetMino(game);
                    if (isHit(game, game->minoX, game->minoY, game->minoType, game->minoAngle)) {
                        game->is_gameover = 1;
                        if (game->port_id == 0) g_system_phase = PHASE_RESULT;
                        fprintf(game->fp_out, "\a"); show_gameover_message(game); wait_retry(game); return; 
                    }
                    game->next_drop_time = tick + g_current_drop_interval;
                } else {
                    /* 接地していなければ1段下げる */
                    game->minoY++;
                    game->next_drop_time = tick + g_current_drop_interval;
                }
                display(game);
                break;
            default: break;
        }
    }
}

/* ***************************************************************************
 * 11. ターボ・オーバードライブ管理タスク
 * *************************************************************************** */

/* ---------------------------------------------------------------------------
 * 関数名 : task_turbo_monitor
 * 概要   : 時間経過監視と難易度・LED制御 (ストップウォッチ方式)
 * 詳細   : 
 * システムフェーズ(g_system_phase)を監視し、プレイ中の場合のみ時間を累積する。
 * 累積時間に基づいて難易度レベルを決定し、落下速度とスコア倍率を更新する。
 * また、レベルに応じてLEDの点灯制御を行う。
 * --------------------------------------------------------------------------- */
void task_turbo_monitor(void) {
    unsigned long current_turbo_ticks = 0; /* 累積経過時間 */
    int i;
    int flash_state = 0;
    
    /* 点滅用カウンタ */
    int blink_wait_counter = 0;

    while (1) {
        /* --- 状態に応じた処理 --- */
        switch (g_system_phase) {
            case PHASE_IDLE:
            case PHASE_COUNTDOWN:
                /* リセット状態: 時間を0に戻し、LEDを消灯 */
                current_turbo_ticks = 0;
                g_current_drop_interval = TURBO_BASE_INTERVAL;
                g_score_multiplier = 1;
                /* リセット時はカウンタもクリア */
                blink_wait_counter = 0;
                flash_state = 0;
                for (i = 0; i < 8; i++) *leds[i] = ' ';
                break;

            case PHASE_PLAYING:
                /* プレイ中: 時間を加算する */
                current_turbo_ticks += TURBO_UPDATE_PERIOD;
                /* フォールスルーしてLED更新へ */
                goto UPDATE_LEVEL;

            case PHASE_RESULT:
                /* 結果表示中: 時間は進めないが、LEDの状態は維持 (更新処理のみ) */
                goto UPDATE_LEVEL;
        }

        /* 待機してループ先頭へ (IDLE/COUNTDOWNの場合はここでcontinue) */
        {
            unsigned long wake = tick + TURBO_UPDATE_PERIOD;
            while (tick < wake) skipmt();
            continue;
        }

    UPDATE_LEVEL:
        /* --- レベル計算とパラメータ更新 --- */
        /* 秒換算 */
        unsigned long elapsed_sec = current_turbo_ticks / TURBO_TICKS_PER_SEC;
        
        /* レベル計算: (経過秒 / MAX到達秒) * 8 */
        int level = (elapsed_sec * 8) / TURBO_MAX_LEVEL_TIME_SEC;
        if (level > 8) level = 8;

        /* パラメータ適用 (定数を使用) */
        switch (level) {
            case 0: case 1: case 2: /* 通常 */
                g_current_drop_interval = TURBO_BASE_INTERVAL;
                g_score_multiplier = 1;
                break;
            case 3: case 4: case 5: /* 高速 (1.5倍) */
                g_current_drop_interval = (TURBO_BASE_INTERVAL * 2) / 3;
                g_score_multiplier = 2;
                break;
            case 6: case 7:         /* ターボ (3.0倍) */
                g_current_drop_interval = TURBO_BASE_INTERVAL / 3;
                g_score_multiplier = 4;
                break;
            case 8:                 /* MAX (6.0倍) */
                g_current_drop_interval = TURBO_BASE_INTERVAL / 6;
                g_score_multiplier = 8;
                break;
        }

        /* LED出力 */
        if (level < 8) {
            /* 通常時はレベルメーター表示 */
            for (i = 0; i < 8; i++) {
                if (i < level) *leds[i] = '#'; /* 点灯 */
                else           *leds[i] = ' '; /* 消灯 */
            }
            /* 点滅状態をリセットしておく */
            blink_wait_counter = 0;
            flash_state = 0; 
        } else {
            /* MAX時の点滅演出 (調整パラメータを使用) */
            blink_wait_counter++;
            
            /* 設定されたサイクル数を超えたら反転 */
            if (blink_wait_counter >= TURBO_BLINK_CYCLE) {
                flash_state = !flash_state;
                blink_wait_counter = 0;
            }
            
            for (i = 0; i < 8; i++) *leds[i] = flash_state ? '#' : ' ';
        }

        /* 待機 */
        unsigned long wake = tick + TURBO_UPDATE_PERIOD;
        while (tick < wake) skipmt();
    }
}

/* ***************************************************************************
 * 12. メインエントリ
 * *************************************************************************** */

/* プレイヤー1用タスク */
void task1(void) {
    TetrisGame game1;
    game1.port_id = 0; game1.fp_out = com0out;
    game1.sync_generation = 0; all_games[0] = &game1; 
    wait_start(&game1);
    while(1) { run_tetris(&game1); }
}

/* プレイヤー2用タスク */
void task2(void) {
    TetrisGame game2;
    game2.port_id = 1; game2.fp_out = com1out;
    game2.sync_generation = 0; all_games[1] = &game2;
    wait_start(&game2);
    while(1) { run_tetris(&game2); }
}

/* メイン関数 */
int main(void) {
    /* カーネル初期化 */
    init_kernel();
    
    /* セマフォ初期化 */
    semaphore[SEM_GARBAGE_LOCK].count = 1;

    /* ストリーム初期化 (csys68k.cに依存) */
    com0in  = fdopen(0, "r"); com0out = fdopen(1, "w");
    com1in  = fdopen(4, "r"); com1out = fdopen(4, "w");
    
    /* タスク登録 */
    set_task(task1);
    set_task(task2);
    set_task(task_turbo_monitor); 
    
    /* マルチタスク開始 */
    begin_sch();
    return 0; /* ここには到達しない */
}