/* ===================================================================
 * tetris_main.c
 * テーマ3対応: イベント駆動型マルチタスク・テトリス
 * * 概要:
 * MC68VZ328用マルチタスクカーネル上で動作する2人対戦テトリス．
 * シリアルポート(UART)経由でVT100互換ターミナルに画面を描画する．
 * * 主な機能:
 * - 7-Bagシステムによるミノ生成
 * - 差分更新による描画最適化 (通信量削減)
 * - ノンブロッキング入力と skipmt() による協調的マルチタスク
 * - 起動時・リトライ時の同期処理
 * =================================================================== */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mtk_c.h"

/* -------------------------------------------------------------------
 * 外部変数・関数の宣言
 * ------------------------------------------------------------------- */
/* ファイルポインタ (csys68k.c 等で定義) */
extern FILE *com0in;
extern FILE *com0out;
extern FILE *com1in;
extern FILE *com1out;

/* カーネル関数・システムコール (mtk_c.c / inchrw.s 等) */
extern void init_kernel(void);
extern void set_task(void (*func)());
extern void begin_sch(void);
extern int inbyte(int ch);
extern void skipmt(void);

/* タイマーティックカウンタ (mtk_c.c) */
extern volatile unsigned long tick; 

/* -------------------------------------------------------------------
 * 定数・マクロ定義
 * ------------------------------------------------------------------- */
/* フィールドサイズ */
#define FIELD_WIDTH  12
#define FIELD_HEIGHT 22

/* ミノの最大サイズ (4x4) */
#define MINO_WIDTH   4
#define MINO_HEIGHT  4

/* 画面レイアウト */
#define OPPONENT_OFFSET_X 40    /* 相手画面の表示Xオフセット */

/* ゲームバランス設定 */
#define DROP_INTERVAL 600     /* 自然落下の間隔 (tick単位) */
#define ANIMATION_DURATION 3   /* ライン消去演出の時間 (tick単位) */

/* システム設定 */
#define DISPLAY_POLL_INTERVAL 50 /* 描画更新の間引き (CPU負荷軽減用) */

/* VT100 エスケープシーケンス (画面制御) */
#define ESC_CLS      "\x1b[2J"      /* 画面クリア */
#define ESC_HOME     "\x1b[H"       /* カーソルをホームへ */
#define ESC_RESET    "\x1b[0m"      /* 属性リセット */
#define ESC_HIDE_CUR "\x1b[?25l"    /* カーソル非表示 */
#define ESC_SHOW_CUR "\x1b[?25h"    /* カーソル表示 */
#define ESC_CLR_LINE "\x1b[K"       /* 行末まで消去 */

/* 演出用エスケープシーケンス */
#define ESC_INVERT_ON  "\x1b[?5h"   /* 画面反転 (フラッシュ) */
#define ESC_INVERT_OFF "\x1b[?5l"   /* 画面反転解除 */

/* TrueColor エスケープシーケンス定義 */
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
 * 型定義
 * ------------------------------------------------------------------- */

/* ゲームの進行状態 */
typedef enum {
    GS_PLAYING,     /* 通常プレイ中 (操作可能) */
    GS_ANIMATING,   /* ライン消去演出中 (操作不可・ウェイト) */
    GS_GAMEOVER     /* ゲームオーバー */
} GameState;

/* イベントの種類 */
typedef enum {
    EVT_NONE,       /* イベントなし */
    EVT_KEY_INPUT,  /* キー入力あり */
    EVT_TIMER,      /* 落下タイマー発火 */
    EVT_WIN,        /* 勝利 (相手がゲームオーバー) */
    EVT_QUIT,       /* 中断要求 */
    EVT_REFRESH     /* 画面更新 (必要に応じて) */
} EventType;

/* イベント構造体 */
typedef struct {
    EventType type;
    int param;      /* キーコードなどの付加情報 */
} Event;

/* * テトリスのゲームコンテキスト構造体
 * 1人のプレイヤーに必要な全ての情報を保持する
 */
typedef struct {
    /* 入出力設定 */
    int port_id;        /* ポート番号 (0 or 1) */
    FILE *fp_out;       /* 出力用ファイルポインタ */
    
    /* フィールドバッファ */
    char field[FIELD_HEIGHT][FIELD_WIDTH];          /* 論理フィールド (固定されたブロック) */
    char displayBuffer[FIELD_HEIGHT][FIELD_WIDTH];  /* 描画用バッファ (現在フレーム) */
    char prevBuffer[FIELD_HEIGHT][FIELD_WIDTH];     /* 前回描画したバッファ (差分検出用) */
    
    /* 対戦相手の画面同期用 */
    char prevOpponentBuffer[FIELD_HEIGHT][FIELD_WIDTH]; 
    int opponent_was_connected;

    /* 状態管理 */
    GameState state;
    unsigned long anim_start_tick;  /* アニメーション開始時刻 */
    int lines_to_clear;             /* 消去予定のライン数 */

    /* 操作中のミノ情報 */
    int minoType;   /* 種類 */
    int minoAngle;  /* 回転角度 (0-3) */
    int minoX;      /* X座標 */
    int minoY;      /* Y座標 */
    
    /* ミノ生成 (7-Bag システム) */
    int bag[7];
    int bag_index;

    /* タイマー管理 */
    unsigned long next_drop_time;
    
    /* 入力解析用ステート (エスケープシーケンス判定) */
    int seq_state;
    
    /* スコア・状況 */
    int score;
    int lines_cleared;
    
    /* 対戦・同期フラグ (volatile必須) */
    volatile int pending_garbage;   /* 相手から送られた未処理のお邪魔ライン */
    volatile int is_gameover;       /* 自身がゲームオーバーになったか */
    volatile int sync_generation;   /* ゲーム開始/リトライの同期用カウンタ */

} TetrisGame;

/* 全ゲームインスタンスへのポインタ (対戦相手参照用) */
TetrisGame *all_games[2] = {NULL, NULL};

/* -------------------------------------------------------------------
 * グローバルデータ (ミノ形状・色定義)
 * ------------------------------------------------------------------- */
enum { 
    MINO_TYPE_I, MINO_TYPE_O, MINO_TYPE_S, MINO_TYPE_Z, 
    MINO_TYPE_J, MINO_TYPE_L, MINO_TYPE_T, 
    MINO_TYPE_GARBAGE, /* お邪魔ブロック用 */
    MINO_TYPE_MAX 
};

const char* minoColors[MINO_TYPE_MAX] = { 
    COL_CYAN, COL_YELLOW, COL_GREEN, COL_RED, 
    COL_BLUE, COL_ORANGE, COL_PURPLE, COL_GRAY 
};

enum { MINO_ANGLE_0, MINO_ANGLE_90, MINO_ANGLE_180, MINO_ANGLE_270, MINO_ANGLE_MAX };

/* * ミノ形状データ (4x4x4方向)
 * 1: ブロックあり， 0: 空白
 */
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
	/* MINO_TYPE_GARBAGE (形状定義なし) */
    { {{0}},{{0}},{{0}},{{0}} }
};

/* -------------------------------------------------------------------
 * 描画ヘルパー関数
 * ------------------------------------------------------------------- */

/*
 * print_cell_content
 * 1セル分の内容をエスケープシーケンス付きで出力する
 */
void print_cell_content(FILE *fp, char cellVal) {
    if (cellVal == 0) {
        /* 空白: ドット表示 */
        fprintf(fp, "%s・%s", BG_BLACK, ESC_RESET);
    } else if (cellVal == 1) {
        /* 壁: 白色ブロック */
        fprintf(fp, "%s%s■%s", BG_BLACK, COL_WALL, ESC_RESET);
    } else if (cellVal >= 2 && cellVal <= 9) {
        /* ミノ: 各色ブロック */
        fprintf(fp, "%s%s■%s", BG_BLACK, minoColors[cellVal - 2], ESC_RESET);
    } else {
        /* エラー表示 */
        fprintf(fp, "??");
    }
}

/* -------------------------------------------------------------------
 * display: 画面描画関数 (最適化版)
 * 概要:
 * 現在のフィールドと落下中のミノを合成し，
 * 前回描画内容と異なる部分のみを出力する(差分更新)．
 * これによりシリアル通信(38400bps)の帯域を節約する．
 * ------------------------------------------------------------------- */
void display(TetrisGame *game) {
    int i, j;
    int changes = 0;
    
    /* 対戦相手の特定 */
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    TetrisGame *opponent = all_games[opponent_id];

    /* 相手が接続されたら，相手画面の強制再描画フラグを立てる */
    int opponent_connected = (opponent != NULL);
    if (opponent_connected && !game->opponent_was_connected) {
        memset(game->prevOpponentBuffer, -1, sizeof(game->prevOpponentBuffer));
    }
    game->opponent_was_connected = opponent_connected;

    /* 1. 描画バッファの作成 (フィールド + 現在のミノ) */
    memcpy(game->displayBuffer, game->field, sizeof(game->field));
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (game->minoType != MINO_TYPE_GARBAGE && 
                minoShapes[game->minoType][game->minoAngle][i][j]) {
                /* フィールド範囲内ならバッファに書き込む */
                if (game->minoY + i >= 0 && game->minoY + i < FIELD_HEIGHT &&
                    game->minoX + j >= 0 && game->minoX + j < FIELD_WIDTH) {
                    game->displayBuffer[game->minoY + i][game->minoX + j] = 2 + game->minoType;
                }
            }
        }
    }

    /* 2. ヘッダ情報(スコア等)の描画 */
    fprintf(game->fp_out, "\x1b[1;1H"); /* カーソルを(1，1)へ */
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
    fprintf(game->fp_out, "%s", ESC_CLR_LINE); /* 行末消去 */

    fprintf(game->fp_out, "\n--------------------------");
    if (opponent != NULL) {
        fprintf(game->fp_out, "\x1b[2;%dH", OPPONENT_OFFSET_X);
        fprintf(game->fp_out, "--------------------------");
    }
    fprintf(game->fp_out, "%s", ESC_CLR_LINE);

    /* 3. フィールド描画 (差分更新) */
    int base_y = 3;

    for (i = 0; i < FIELD_HEIGHT; i++) {
        /* --- 左側: 自分 --- */
        for (j = 0; j < FIELD_WIDTH; j++) {
            char myVal = game->displayBuffer[i][j];
            /* 前回と値が違う場合のみ描画 */
            if (myVal != game->prevBuffer[i][j]) {
                fprintf(game->fp_out, "\x1b[%d;%dH", base_y + i, j * 2 + 1);
                print_cell_content(game->fp_out, myVal);
                game->prevBuffer[i][j] = myVal;
                changes++;
            }
        }

        /* --- 右側: 相手 --- */
        if (opponent != NULL) {
            for (j = 0; j < FIELD_WIDTH; j++) {
                char oppVal = opponent->displayBuffer[i][j];
                /* 相手画面も差分のみ更新 */
                if (oppVal != game->prevOpponentBuffer[i][j]) {
                    fprintf(game->fp_out, "\x1b[%d;%dH", base_y + i, OPPONENT_OFFSET_X + j * 2);
                    print_cell_content(game->fp_out, oppVal);
                    game->prevOpponentBuffer[i][j] = oppVal;
                    changes++;
                }
            }
        }
    }
    
    /* 変更があった場合のみバッファフラッシュ */
    if (changes > 0) fflush(game->fp_out);
}

/* -------------------------------------------------------------------
 * wait_event: イベント待機・処理関数
 * 概要:
 * - ノンブロッキング入力 (inbyte) を監視する．
 * - 入力がなければ skipmt() を呼び，他タスクにCPU権を譲る．
 * - 矢印キーのエスケープシーケンス(ESC [ A など)を解析する．
 * - 一定時間経過(tick)でタイマーイベントを返す．
 * ------------------------------------------------------------------- */
Event wait_event(TetrisGame *game) {
    Event e;
    e.type = EVT_NONE;
    int c;
    int opponent_id = (game->port_id == 0) ? 1 : 0;
    static int poll_counter = 0;

    while (1) {
        /* 相手が負けたかチェック */
        if (all_games[opponent_id] != NULL && all_games[opponent_id]->is_gameover) {
            e.type = EVT_WIN;
            return e;
        }

        /* ノンブロッキング入力取得 */
        c = inbyte(game->port_id);
        if (c != -1) {
            /* キー入力あり: エスケープシーケンスのステートマシン処理 */
            if (game->seq_state == 0) {
                if (c == 0x1b) game->seq_state = 1;     /* ESC受信 */
                else if (c == 'q') { e.type = EVT_QUIT; return e; }
                else { e.type = EVT_KEY_INPUT; e.param = c; return e; }
            } 
            else if (game->seq_state == 1) {
                if (c == '[') game->seq_state = 2;      /* [ 受信 */
                else game->seq_state = 0;
            } 
            else if (game->seq_state == 2) {
                /* コマンド受信 */
                game->seq_state = 0;
                switch (c) {
                    case 'A': e.param = 'w'; break; /* Up */
                    case 'B': e.param = 's'; break; /* Down */
                    case 'C': e.param = 'd'; break; /* Right */
                    case 'D': e.param = 'a'; break; /* Left */
                    default:  e.param = 0;   break;
                }
                if (e.param != 0) { e.type = EVT_KEY_INPUT; return e; }
            }
        } 
        else {
            /* 入力なし: タイマー判定 */
            if (tick >= game->next_drop_time) {
                e.type = EVT_TIMER;
                return e;
            }
            
            /* 画面の定期更新 (ポーリング頻度を落とす) */
            poll_counter++;
            if (poll_counter >= DISPLAY_POLL_INTERVAL) {
                display(game); 
                poll_counter = 0;
                
                /* アニメーション中ならイベントなしでリターンして処理を進める */
                if (game->state == GS_ANIMATING) {
                    e.type = EVT_NONE; 
                    return e; 
                }
            }
            
            /* 処理することがないのでタスク切り替え (CPUを譲る) */
            skipmt();
        }
    }
}

/* -------------------------------------------------------------------
 * ゲームロジック関数群
 * ------------------------------------------------------------------- */

/* 当たり判定 (1:衝突， 0:なし) */
int isHit(TetrisGame *game, int _minoX, int _minoY, int _minoType, int _minoAngle) {
    int i, j;
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (minoShapes[_minoType][_minoAngle][i][j]) {
                int fy = _minoY + i;
                int fx = _minoX + j;
                /* 壁・床判定 */
                if (fy < 0 || fy >= FIELD_HEIGHT || fx < 0 || fx >= FIELD_WIDTH) return 1;
                /* 既存ブロック判定 */
                if (game->field[fy][fx]) return 1;
            }
        }
    }
    return 0;
}

/* 7-Bagシステムによるミノ順序生成 (ランダムシャッフル) */
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

/* ミノのリセット・次生成 */
void resetMino(TetrisGame *game) {
    game->minoX = 5;
    game->minoY = 0;
    if (game->bag_index >= 7) fillBag(game);
    game->minoType = game->bag[game->bag_index];
    game->bag_index++;
    game->minoAngle = (tick + rand()) % MINO_ANGLE_MAX;
}

/* お邪魔ライン(Garbage)のせり上がり処理 */
int processGarbage(TetrisGame *game) {
    int lines = game->pending_garbage;
    if (lines <= 0) return 0;
    if (lines > 4) lines = 4;   /* 一度に最大4ラインまで */
    game->pending_garbage -= lines;

    int i, j, k;
    /* 最上部にブロックがあるとゲームオーバー */
    for (k = 0; k < lines; k++) {
        for (j = 1; j < FIELD_WIDTH - 1; j++) {
            if (game->field[k][j] != 0) return 1; 
        }
    }
    /* 全体を上にシフト */
    for (i = 0; i < FIELD_HEIGHT - 1 - lines; i++) {
        memcpy(game->field[i], game->field[i + lines], FIELD_WIDTH);
    }
    /* 最下部に穴あきラインを追加 */
    for (i = FIELD_HEIGHT - 1 - lines; i < FIELD_HEIGHT - 1; i++) {
        game->field[i][0] = 1;
        game->field[i][FIELD_WIDTH - 1] = 1;
        for (j = 1; j < FIELD_WIDTH - 1; j++) {
            game->field[i][j] = 2 + MINO_TYPE_GARBAGE; 
        }
        /* ランダムな位置に穴を空ける */
        int hole = 1 + (tick + rand() + i) % (FIELD_WIDTH - 2);
        game->field[i][hole] = 0;
    }
    return 0; 
}

/* -------------------------------------------------------------------
 * 同期・メッセージ表示関数
 * ------------------------------------------------------------------- */

/* * wait_start: ゲーム開始前の同期と乱数初期化 
 * 修正: 起動直後の乱数固定化を防ぐため，キー入力タイミングでseedを設定
 */
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
    /* これにより，起動直後でも毎回異なるミノ順序になる */
    srand((unsigned int)tick);

    /* 3. 開始同期 (相手が準備完了するのを待つ) */
    game->sync_generation++;
    fprintf(game->fp_out, ESC_CLR_LINE);
    fprintf(game->fp_out, "\rWaiting for opponent...   \n");
    fflush(game->fp_out);

    while (1) {
        if (all_games[opponent_id] != NULL) {
            /* 同期世代が一致したら開始 */
            if (all_games[opponent_id]->sync_generation == game->sync_generation) break;
        } else {
            break; /* 相手がいなければ待たない */
        }
        skipmt();
    }
}

/* リトライ待機処理 */
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

    /* 同期処理 */
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
 * ゲームループ本体
 * ------------------------------------------------------------------- */
void run_tetris(TetrisGame *game) {
    int i, j;
    
    /* --- ゲーム状態の初期化 --- */
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
    
    game->bag_index = 7; /* 初回fillBagを強制 */

    fprintf(game->fp_out, ESC_CLS ESC_HIDE_CUR); 
    
    /* フィールド枠の初期化 */
    memset(game->field, 0, sizeof(game->field));
    for (i = 0; i < FIELD_HEIGHT; i++) {
        game->field[i][0] = game->field[i][FIELD_WIDTH - 1] = 1; /* 左右の壁 */
    }
    for (i = 0; i < FIELD_WIDTH; i++) {
        game->field[FIELD_HEIGHT - 1][i] = 1; /* 床 */
    }

    resetMino(game);
    display(game);
    game->next_drop_time = tick + DROP_INTERVAL;

    /* --- メインループ --- */
    while (1) {
        Event e = wait_event(game);

        /* ライン消去アニメーション中の処理 */
        if (game->state == GS_ANIMATING) {
            if (tick >= game->anim_start_tick + ANIMATION_DURATION) {
                /* アニメーション終了 */
                fprintf(game->fp_out, ESC_INVERT_OFF); 
                
                game->lines_cleared += game->lines_to_clear;
                
                /* 攻撃ライン数の計算 */
                int attack = 0;
                if (game->lines_to_clear == 1) attack = 1;      /* 1列消しは攻撃なし? (ルールによる) */
                if (game->lines_to_clear == 2) attack = 1;
                if (game->lines_to_clear == 3) attack = 2;
                if (game->lines_to_clear == 4) attack = 4;
                
                /* 相手へ攻撃を送る */
                if (attack > 0) {
                    int opponent_id = (game->port_id == 0) ? 1 : 0;
                    if (all_games[opponent_id] != NULL && !all_games[opponent_id]->is_gameover) {
                        all_games[opponent_id]->pending_garbage += attack;
                    }
                }
                
                /* スコア加算 */
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
            continue; /* アニメーション中は操作を受け付けない */
        }

        /* 通常イベント処理 */
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
                /* 操作処理 */
                switch (e.param) {
                    case 's': /* ソフトドロップ */
                        if (!isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                            game->minoY++;
                            game->next_drop_time = tick + DROP_INTERVAL;
                        }
                        break;
                    case 'a': /* 左移動 */
                        if (!isHit(game, game->minoX - 1, game->minoY, game->minoType, game->minoAngle)) game->minoX--;
                        break;
                    case 'd': /* 右移動 */
                        if (!isHit(game, game->minoX + 1, game->minoY, game->minoType, game->minoAngle)) game->minoX++;
                        break;
                    case ' ':
                    case 'w': /* 回転 */
                        if (!isHit(game, game->minoX, game->minoY, game->minoType, (game->minoAngle + 1) % MINO_ANGLE_MAX)) {
                            game->minoAngle = (game->minoAngle + 1) % MINO_ANGLE_MAX;
                        }
                        break;
                }
                display(game);
                break;

            case EVT_TIMER:
                /* 自由落下処理 */
                if (isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                    /* 接地固定 */
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
                    
                    /* ライン消去判定 */
                    int lines_this_turn = 0;
                    for (i = 0; i < FIELD_HEIGHT - 1; i++) {
                        int lineFill = 1;
                        for (j = 1; j < FIELD_WIDTH - 1; j++) {
                            if (game->field[i][j] == 0) { lineFill = 0; break; }
                        }
                        if (lineFill) {
                            /* ラインを詰める */
                            int k;
                            for (k = i; k > 0; k--) memcpy(game->field[k], game->field[k - 1], FIELD_WIDTH);
                            memset(game->field[0], 0, FIELD_WIDTH);
                            game->field[0][0] = game->field[0][FIELD_WIDTH-1] = 1;
                            lines_this_turn++;
                        }
                    }

                    if (lines_this_turn > 0) {
                        /* 消去演出開始 */
                        fprintf(game->fp_out, "\a" ESC_INVERT_ON); 
                        fflush(game->fp_out);
                        game->state = GS_ANIMATING;
                        game->anim_start_tick = tick;
                        game->lines_to_clear = lines_this_turn;
                        break; /* タイマーイベント処理中断してループ先頭へ */
                    }

                PROCESS_GARBAGE:
                    /* お邪魔ラインの処理 */
                    if (processGarbage(game)) {
                        /* せり上がりで窒息 */
                        game->is_gameover = 1;
                        fprintf(game->fp_out, "\a");
                        show_gameover_message(game);
                        wait_retry(game); 
                        return;
                    }
                    
                    /* 次のミノへ */
                    resetMino(game);
                    
                    /* 出現即死亡判定 */
                    if (isHit(game, game->minoX, game->minoY, game->minoType, game->minoAngle)) {
                        game->is_gameover = 1;
                        fprintf(game->fp_out, "\a");
                        show_gameover_message(game);
                        wait_retry(game); 
                        return; 
                    }
                    game->next_drop_time = tick + DROP_INTERVAL;

                } else {
                    /* まだ落ちる余地あり */
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
 * ユーザタスク定義
 * ------------------------------------------------------------------- */

/* タスク1: Port 0 (UART1) 担当 */
void task1(void) {
    TetrisGame game1;
    game1.port_id = 0;
    game1.fp_out = com0out;
    game1.sync_generation = 0;
    all_games[0] = &game1; 
    
    /* 初期化同期 */
    wait_start(&game1);

    while(1) {
        run_tetris(&game1);
    }
}

/* タスク2: Port 1 (UART2) 担当 */
void task2(void) {
    TetrisGame game2;
    game2.port_id = 1;
    game2.fp_out = com1out;
    game2.sync_generation = 0;
    all_games[1] = &game2;
    
    /* 初期化同期 */
    wait_start(&game2);

    while(1) {
        run_tetris(&game2);
    }
}

/* -------------------------------------------------------------------
 * メイン関数
 * ------------------------------------------------------------------- */
int main(void) {
    /* カーネル初期化 */
    init_kernel();
    
    /* シリアルポートのストリーム設定 (Port0, Port1) */
    com0in  = fdopen(0, "r");
    com0out = fdopen(1, "w");
    com1in  = fdopen(4, "r");
    com1out = fdopen(4, "w");

    /* タスクの登録 */
    set_task(task1);
    set_task(task2);

    /* マルチタスクスケジューリング開始 (ここからは戻らない) */
    begin_sch();
    return 0;
}