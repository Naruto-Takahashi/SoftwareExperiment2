/* ===========================================================================
 * ファイル名 : tetris_main.c
 * テーマ     : テーマ3 応用 (ターボ機能付き対戦テトリス - 実機調整版)
 * 作成者     : [Your Name/Group]
 * 修正日     : 2025/12/26 (可読性向上版)
 *
 * [システム概要]
 * MC68VZ328用自作マルチタスクカーネル上で動作する，2人対戦型テトリス．
 * UARTシリアル通信を用いて2つの端末で対戦を行う．
 *
 * [タスク構成]
 * 1. User Task 1 (Port 0) : プレイヤー1のゲームロジック (生産者/消費者)
 * 2. User Task 2 (Port 1) : プレイヤー2のゲームロジック (生産者/消費者)
 * 3. Turbo Task           : 時間経過を監視し，難易度調整とLED演出を行う (管理者)
 *
 * [機能特徴]
 * - 共有メモリとセマフォを用いたお邪魔ブロックの攻撃システム
 * - 経過時間による動的な難易度上昇 (速度 & スコア倍率)
 * - 差分更新による高速な画面描画
 * =========================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mtk_c.h"

/* ***************************************************************************
 * 1. ハードウェア定義 & 定数マクロ
 * *************************************************************************** */

/* --- メモリマップ定義 (equdefs.inc 準拠) --- */
#define IOBASE  0x00D00000

/* * LEDアドレス配列 
 * 実機仕様に合わせ，IOBASEからのオフセットで各LEDのアドレスを定義．
 * 文字コードを書き込むことで点灯制御を行う．
 */
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

/* --- 外部関数の宣言 (カーネル/ライブラリ) --- */
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

/* --- 共有グローバル変数 (Turbo Task管理) --- */
/* Turbo Taskが書き込み，Game Taskが読み込むため volatile 推奨 */
volatile unsigned long g_current_drop_interval = 500; /* 現在の落下待機時間 (tick) */
volatile int g_score_multiplier = 1;                  /* 現在のスコア倍率 */

/* --- セマフォID定義 --- */
#define SEM_GARBAGE_LOCK 0  /* お邪魔ライン送受信の排他制御用 (Mutex) */

/* --- ゲームパラメータ設定 --- */
#define FIELD_WIDTH  12       /* 壁を含むフィールド幅 */
#define FIELD_HEIGHT 22       /* 壁を含むフィールド高さ */
#define MINO_WIDTH   4        /* ミノデータの幅 */
#define MINO_HEIGHT  4        /* ミノデータの高さ */
#define OPPONENT_OFFSET_X 40  /* 相手画面の表示X座標オフセット */
#define ANIMATION_DURATION 3  /* ライン消去演出の長さ (tick) */
#define COUNTDOWN_DELAY 1000  /* カウントダウンの待機時間 */
#define DISPLAY_POLL_INTERVAL 50 /* 入力待ち時の描画更新頻度 */

/* --- 速度・難易度調整パラメータ --- */
#define BASE_DROP_INTERVAL 500 /* 基準となる落下間隔 (tick) */
#define TIME_TO_MAX_LEVEL 120  /* 最高レベル到達までの時間 (秒) */
#define TICKS_PER_SEC     100  /* 1秒あたりのtick数 */

/* --- フィールドセル値 --- */
#define CELL_EMPTY  0
#define CELL_WALL   1
#define CELL_GHOST  10

/* --- エスケープシーケンス定義 --- */
#define ESC_CLS        "\x1b[2J"    /* 画面消去 */
#define ESC_HOME       "\x1b[H"     /* カーソルホーム */
#define ESC_RESET      "\x1b[0m"    /* 属性リセット */
#define ESC_HIDE_CUR   "\x1b[?25l"  /* カーソル非表示 */
#define ESC_SHOW_CUR   "\x1b[?25h"  /* カーソル表示 */
#define ESC_CLR_LINE   "\x1b[K"     /* 行消去 */
#define ESC_INVERT_ON  "\x1b[?5h"   /* 画面反転ON (フラッシュ演出用) */
#define ESC_INVERT_OFF "\x1b[?5l"   /* 画面反転OFF */

/* --- カラー定義 (RGB指定) --- */
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
 * 2. 構造体・データ型定義
 * *************************************************************************** */

/* ゲームの状態 */
typedef enum { 
    GS_PLAYING,   /* プレイ中 */
    GS_ANIMATING, /* ライン消去アニメーション中 */
    GS_GAMEOVER   /* ゲームオーバー */
} GameState;

/* イベントの種類 */
typedef enum { 
    EVT_NONE,      /* なし */
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
    /* 通信・出力設定 */
    int port_id;   /* 0 or 1 */
    FILE *fp_out;  /* 出力ストリーム */
    
    /* フィールドデータ */
    char field[FIELD_HEIGHT][FIELD_WIDTH];
    char displayBuffer[FIELD_HEIGHT][FIELD_WIDTH];      /* 現在の描画内容 */
    char prevBuffer[FIELD_HEIGHT][FIELD_WIDTH];         /* 前回の描画内容 (自分用) */
    char prevOpponentBuffer[FIELD_HEIGHT][FIELD_WIDTH]; /* 前回の描画内容 (相手用) */
    int opponent_was_connected;                         /* 相手接続フラグ */

    /* 状態管理 */
    GameState state;
    unsigned long anim_start_tick;
    int lines_to_clear;

    /* ミノ制御 */
    int minoType, minoAngle, minoX, minoY;
    int nextMinoType, prevNextMinoType;
    int bag[7], bag_index; /* 7種1巡のランダム生成用 */

    /* タイミング・入力制御 */
    unsigned long next_drop_time;
    int seq_state; /* エスケープシーケンス解析用ステート */
    
    /* スコア・統計 */
    int score;
    int lines_cleared;
    
    /* --- 共有資源 (セマフォ保護対象) --- */
    volatile int pending_garbage; /* 受け取ったお邪魔ライン数 */
    volatile int is_gameover;     /* ゲームオーバーフラグ */
    volatile int sync_generation; /* 同期用世代カウンタ */
} TetrisGame;

/* 全ゲームインスタンスへのポインタ (相手参照用) */
TetrisGame *all_games[2] = {NULL, NULL};

/* ミノ定義 */
enum { MINO_TYPE_I, MINO_TYPE_O, MINO_TYPE_S, MINO_TYPE_Z, MINO_TYPE_J, MINO_TYPE_L, MINO_TYPE_T, MINO_TYPE_GARBAGE, MINO_TYPE_MAX };
enum { MINO_ANGLE_0, MINO_ANGLE_90, MINO_ANGLE_180, MINO_ANGLE_270, MINO_ANGLE_MAX };

const char* minoColors[MINO_TYPE_MAX] = { 
    COL_CYAN, COL_YELLOW, COL_GREEN, COL_RED, COL_BLUE, COL_ORANGE, COL_PURPLE, COL_GRAY 
};

/* ミノ形状データ [種類][角度][y][x] */
char minoShapes[MINO_TYPE_MAX][MINO_ANGLE_MAX][MINO_HEIGHT][MINO_WIDTH] = {
    /* Iミノ */
    { {{0,1,0,0},{0,1,0,0},{0,1,0,0},{0,1,0,0}}, {{0,0,0,0},{0,0,0,0},{1,1,1,1},{0,0,0,0}}, {{0,0,1,0},{0,0,1,0},{0,0,1,0},{0,0,1,0}}, {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}} },
    /* Oミノ */
    { {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,1,1,0},{0,0,0,0}} },
    /* Sミノ */
    { {{0,0,0,0},{0,1,1,0},{1,1,0,0},{0,0,0,0}}, {{0,1,0,0},{0,1,1,0},{0,0,1,0},{0,0,0,0}}, {{0,0,0,0},{0,0,1,1},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,0,0},{0,1,1,0},{0,0,1,0}} },
    /* Zミノ */
    { {{0,0,0,0},{1,1,0,0},{0,1,1,0},{0,0,0,0}}, {{0,0,1,0},{0,1,1,0},{0,1,0,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,0,1,1},{0,0,0,0}}, {{0,0,0,0},{0,0,1,0},{0,1,1,0},{0,1,0,0}} },
    /* Jミノ */
    { {{0,0,1,0},{0,0,1,0},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,0,0},{0,1,1,1},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,1,0,0},{0,1,0,0}}, {{0,0,0,0},{1,1,1,0},{0,0,1,0},{0,0,0,0}} },
    /* Lミノ */
    { {{0,1,0,0},{0,1,0,0},{0,1,1,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,1},{0,1,0,0},{0,0,0,0}}, {{0,0,0,0},{0,1,1,0},{0,0,1,0},{0,0,1,0}}, {{0,0,0,0},{0,0,1,0},{1,1,1,0},{0,0,0,0}} },
    /* Tミノ */
    { {{0,0,0,0},{1,1,1,0},{0,1,0,0},{0,0,0,0}}, {{0,0,1,0},{0,1,1,0},{0,0,1,0},{0,0,0,0}}, {{0,0,0,0},{0,0,1,0},{0,1,1,1},{0,0,0,0}}, {{0,0,0,0},{0,1,0,0},{0,1,1,0},{0,1,0,0}} },
    /* GARBAGE (未使用) */
    { {{0}},{{0}},{{0}},{{0}} }
};

/* ***************************************************************************
 * 3. 関数プロトタイプ宣言
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
 * 4. 描画・表示関連関数
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
 * 概要   : 画面全体の描画処理 (ダブルバッファリングによる差分更新)
 * 手順   :
 * 1. 相手プレイヤーの接続状態を確認し，必要ならバッファを初期化．
 * 2. 現在のフィールド状態と操作中ミノから，描画バッファを構築．
 * 3. ヘッダ情報 (スコア，攻撃保留数など) を描画．
 * 4. 前回描画した内容と比較し，変更があったセルのみカーソル移動して再描画．
 * --------------------------------------------------------------------------- */
void display(TetrisGame *game) {
    int i, j;
    int changes = 0;
    
    /* 対戦相手の取得 */
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    TetrisGame *opponent = all_games[opponent_id];

    /* 相手接続検知時のバッファリセット */
    int opponent_connected = (opponent != NULL);
    if (opponent_connected && !game->opponent_was_connected) {
        memset(game->prevOpponentBuffer, -1, sizeof(game->prevOpponentBuffer));
    }
    game->opponent_was_connected = opponent_connected;

    /* --- [Step 1] 描画バッファ構築 --- */
    /* ベースフィールドのコピー */
    memcpy(game->displayBuffer, game->field, sizeof(game->field));

    /* ゴースト (落下予想位置) の描画 */
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

    /* --- [Step 2] ヘッダ情報の描画 --- */
    /* カーソルをホームへ移動し，ステータス行を描画 */
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
    fprintf(game->fp_out, "%s", ESC_CLR_LINE); /* 行末消去 */

    fprintf(game->fp_out, "\n--------------------------");
    if (opponent != NULL) {
        fprintf(game->fp_out, "\x1b[2;%dH", OPPONENT_OFFSET_X);
        fprintf(game->fp_out, "--------------------------");
    }
    fprintf(game->fp_out, "%s", ESC_CLR_LINE);

    /* --- [Step 3] フィールドの差分描画 --- */
    int base_y = 3;
    for (i = 0; i < FIELD_HEIGHT; i++) {
        /* 自分自身のフィールド */
        for (j = 0; j < FIELD_WIDTH; j++) {
            char myVal = game->displayBuffer[i][j];
            if (myVal != game->prevBuffer[i][j]) {
                /* 変更があった箇所のみカーソル移動して描画 */
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
        
        /* 1秒 (1000ms) 待機 */
        unsigned long target_tick = tick + COUNTDOWN_DELAY;
        while (tick < target_tick) skipmt();
    }
    /* カウントダウン表示を消すために前回のバッファを無効化 */
    memset(game->prevBuffer, -1, sizeof(game->prevBuffer));
}

/* ***************************************************************************
 * 5. イベント・入力制御関数
 * *************************************************************************** */

/* ---------------------------------------------------------------------------
 * 関数名 : wait_event
 * 概要   : イベント待機ループ (ノンブロッキング入力)
 * 戻り値 : 発生したイベント構造体
 * 備考   : キー入力，タイマ発火，勝利判定などを監視する
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

        /* 2. キー入力チェック (ノンブロッキング) */
        c = inbyte(game->port_id);
        if (c != -1) {
            /* エスケープシーケンスの解析ステートマシン */
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
                /* 矢印キーの変換 */
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
            
            /* 4. アイドル時の定期描画更新 (相手画面の更新反映用) */
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
 * 6. ゲームロジック関数
 * *************************************************************************** */

/* ---------------------------------------------------------------------------
 * 関数名 : isHit
 * 概要   : 当たり判定を行う
 * 戻り値 : 1 = 衝突あり, 0 = なし
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
 * 概要   : 7種1巡の法則に従い，次のミノ列を生成する (Fisher-Yates shuffle)
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
 * 概要   : 新しいミノを出現させる
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
 * 概要   : 相手から送られたお邪魔ブロックを処理する
 * 手順   :
 * 1. セマフォで排他制御し，pending_garbage を読み出す．
 * 2. 一度に処理する最大ライン数は4とする．
 * 3. フィールド全体を上にシフトし，最下段に穴あきブロック列を追加．
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
    /* 下段にお邪魔ライン生成 */
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
 * 7. 同期・メッセージ表示関数
 * *************************************************************************** */

/* ---------------------------------------------------------------------------
 * 関数名 : wait_start
 * 概要   : ゲーム開始時の同期待機
 * 手順   :
 * 1. スタート画面を表示し，キー入力を待つ．
 * 2. 乱数シードを初期化．
 * 3. 共有変数 sync_generation を用いて，相手も準備完了になるのを待つ．
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

    /* 相手との同期 */
    while (1) {
        if (all_games[opponent_id] != NULL) {
            if (all_games[opponent_id]->sync_generation == game->sync_generation) break;
        } else break; /* 相手がいない場合は即開始 */
        skipmt();
    }
}

/* ---------------------------------------------------------------------------
 * 関数名 : wait_retry
 * 概要   : ゲーム終了後のリトライ待機 (wait_startと同様の同期機構)
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
 * 8. メインゲームループ
 * *************************************************************************** */

/* ---------------------------------------------------------------------------
 * 関数名 : run_tetris
 * 概要   : テトリスのメインループ処理
 * 手順   :
 * 1. 変数・フィールドの初期化．
 * 2. ミノの生成とカウントダウン．
 * 3. イベントループ開始:
 * a. アニメーション処理 (完了時にスコア加算・お邪魔送信)．
 * b. キー入力処理 (移動・回転)．
 * c. 自然落下処理 (接地判定・ライン消去判定)．
 * d. ゲームオーバー判定．
 * --------------------------------------------------------------------------- */
void run_tetris(TetrisGame *game) {
    int i;
    
    /* 1. 変数初期化 */
    game->score = 0; game->lines_cleared = 0; game->pending_garbage = 0;
    game->is_gameover = 0; game->state = GS_PLAYING; 
    game->lines_to_clear = 0; game->seq_state = 0;
    game->opponent_was_connected = 0; game->prevNextMinoType = -1; 
    
    /* 画面バッファリセット */
    memset(game->prevBuffer, -1, sizeof(game->prevBuffer));
    memset(game->prevOpponentBuffer, -1, sizeof(game->prevOpponentBuffer));
    
    fprintf(game->fp_out, ESC_CLS ESC_HIDE_CUR); 
    
    /* フィールド初期化 (枠作成) */
    memset(game->field, 0, sizeof(game->field));
    for (i = 0; i < FIELD_HEIGHT; i++) game->field[i][0] = game->field[i][FIELD_WIDTH - 1] = 1; 
    for (i = 0; i < FIELD_WIDTH; i++) game->field[FIELD_HEIGHT - 1][i] = 1; 

    /* ミノ生成 */
    fillBag(game); game->nextMinoType = game->bag[game->bag_index++]; resetMino(game); 
    
    display(game);
    perform_countdown(game); /* カウントダウン */
    display(game);
    
    /* 落下タイマー設定 (共有変数 g_current_drop_interval を使用) */
    game->next_drop_time = tick + g_current_drop_interval;

    /* 2. メインループ */
    while (1) {
        Event e = wait_event(game);

        /* アニメーション状態の処理 */
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

        /* イベント処理 */
        switch (e.type) {
            case EVT_WIN:
                show_victory_message(game); wait_retry(game); return; 
            case EVT_QUIT:
                fprintf(game->fp_out, "%sQuit.\n", ESC_SHOW_CUR); wait_retry(game); return;
            case EVT_KEY_INPUT:
                /* キー操作 (WASD / Space) */
                switch (e.param) {
                    case 's': /* ソフトドロップ */
                        if (!isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                            game->minoY++; game->next_drop_time = tick + g_current_drop_interval;
                        }
                        break;
                    case 'a': /* 左移動 */
                        if (!isHit(game, game->minoX - 1, game->minoY, game->minoType, game->minoAngle)) game->minoX--;
                        break;
                    case 'd': /* 右移動 */
                        if (!isHit(game, game->minoX + 1, game->minoY, game->minoType, game->minoAngle)) game->minoX++;
                        break;
                    case ' ': /* 回転 */
                        {
                            int newAngle = (game->minoAngle + 1) % MINO_ANGLE_MAX;
                            if (!isHit(game, game->minoX, game->minoY, game->minoType, newAngle)) game->minoAngle = newAngle;
                            else if (!isHit(game, game->minoX + 1, game->minoY, game->minoType, newAngle)) { game->minoX++; game->minoAngle = newAngle; }
                            else if (!isHit(game, game->minoX - 1, game->minoY, game->minoType, newAngle)) { game->minoX--; game->minoAngle = newAngle; }
                        }
                        break;
                    case 'w': /* ハードドロップ */
                        while (!isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                            game->minoY++; game->score += 2 * g_score_multiplier; 
                        }
                        display(game); goto LOCK_PROCESS; 
                        break;
                }
                display(game);
                break;

            case EVT_TIMER:
                /* 落下処理 */
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
                        fprintf(game->fp_out, "\a" ESC_INVERT_ON); /* ベル音 + 画面フラッシュ */
                        fflush(game->fp_out);
                        game->state = GS_ANIMATING;
                        game->anim_start_tick = tick;
                        game->lines_to_clear = lines_this_turn;
                        break; /* ループ先頭へ戻りアニメーション処理へ */
                    }
                    
                PROCESS_GARBAGE: 
                    /* 4. お邪魔ブロックのせり上がり処理 */
                    if (processGarbage(game)) {
                        game->is_gameover = 1;
                        fprintf(game->fp_out, "\a"); show_gameover_message(game); wait_retry(game); return;
                    }
                    
                    /* 5. 次のミノのリセットと出現判定 */
                    resetMino(game);
                    if (isHit(game, game->minoX, game->minoY, game->minoType, game->minoAngle)) {
                        game->is_gameover = 1;
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
 * 9. ターボ・オーバードライブ管理タスク
 * *************************************************************************** */

/* ---------------------------------------------------------------------------
 * 関数名 : task_turbo_monitor
 * 概要   : 時間経過による難易度上昇とLED演出を管理する (独立タスク)
 * 手順   :
 * 1. 経過時間 (秒) を計算．
 * 2. 経過時間からレベル (0-8) を決定．
 * 3. レベルに応じて落下速度 (基準値からの計算) と得点倍率を変更．
 * 4. LEDを制御 (ASCII '#' = 点灯, ' ' = 消灯)．
 * --------------------------------------------------------------------------- */
void task_turbo_monitor(void) {
    unsigned long start_tick = tick;
    unsigned long elapsed_sec = 0;
    int i;
    int flash_state = 0;

    while (1) {
        /* 1. 経過時間の計測 */
        if (tick >= start_tick) elapsed_sec = (tick - start_tick) / TICKS_PER_SEC;
        else elapsed_sec = 0; /* カウンタオーバーフロー対策 (簡易) */

        /* 2. レベル計算 (0-8) */
        int level = (elapsed_sec * 8) / TIME_TO_MAX_LEVEL;
        if (level > 8) level = 8;

        /* 3. パラメータ更新 (基準値を倍率で割って算出) */
        switch (level) {
            case 0: case 1: case 2:
                /* 通常モード (x1.0) */
                g_current_drop_interval = BASE_DROP_INTERVAL;
                g_score_multiplier = 1;
                break;
            case 3: case 4: case 5:
                /* 高速モード (x1.5) */
                g_current_drop_interval = (BASE_DROP_INTERVAL * 2) / 3;
                g_score_multiplier = 2;
                break;
            case 6: case 7:
                /* ターボモード (x3.0) */
                g_current_drop_interval = BASE_DROP_INTERVAL / 3;
                g_score_multiplier = 4;
                break;
            case 8:
                /* オーバードライブ (x6.0) */
                g_current_drop_interval = BASE_DROP_INTERVAL / 6;
                g_score_multiplier = 8;
                break;
        }

        /* 4. LED演出 (ASCII文字による点灯制御) */
        if (level < 8) {
            /* 通常時: ゲージ表示 */
            for (i = 0; i < 8; i++) {
                if (i < level) *leds[i] = '#'; /* 点灯 */
                else           *leds[i] = ' '; /* 消灯 */
            }
        } else {
            /* MAX時: ストロボ点滅 */
            flash_state = !flash_state;
            for (i = 0; i < 8; i++) {
                *leds[i] = flash_state ? '#' : ' ';
            }
        }

        /* 0.2秒待機 (点滅周期) */
        unsigned long wake = tick + 20;
        while (tick < wake) skipmt();
    }
}

/* ***************************************************************************
 * 10. タスクエントリ & メイン関数
 * *************************************************************************** */

/* プレイヤー1用タスク (Port 0) */
void task1(void) {
    TetrisGame game1;
    game1.port_id = 0; game1.fp_out = com0out;
    game1.sync_generation = 0; all_games[0] = &game1; 
    wait_start(&game1);
    while(1) { run_tetris(&game1); }
}

/* プレイヤー2用タスク (Port 1) */
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
    semaphore[SEM_GARBAGE_LOCK].count = 1; /* 初期値1 (Mutexとして使用) */

    /* ファイルディスクリプタ設定 (csys68k.cに依存) */
    com0in  = fdopen(0, "r"); com0out = fdopen(1, "w");
    com1in  = fdopen(4, "r"); com1out = fdopen(4, "w");
    
    /* タスク登録 */
    set_task(task1);
    set_task(task2);
    set_task(task_turbo_monitor); /* ターボ管理タスク起動 */
    
    /* マルチタスクスケジューリング開始 */
    begin_sch();
    
    return 0; /* ここには到達しない */
}