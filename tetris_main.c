/* ===================================================================
 * tetris_main.c
 * M68k実験ボード用 テトリス (カラー対応 & カーソル常時表示版)
 * =================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mtk_c.h"

/* -------------------------------------------------------------------
 * 外部関数の宣言
 * ------------------------------------------------------------------- */
extern void init_kernel(void);
extern void set_task(void (*func)());
extern void begin_sch(void);
extern int inbyte(int ch);      /* ノンブロッキング入力 */
extern void skipmt(void);       /* CPU譲渡 */

/* -------------------------------------------------------------------
 * 定数・マクロ定義
 * ------------------------------------------------------------------- */
#define FIELD_WIDTH  12
#define FIELD_HEIGHT 22

#define MINO_WIDTH   4
#define MINO_HEIGHT  4

/* VT100 エスケープシーケンス */
#define ESC_CLS      "\x1b[2J"      /* 画面クリア */
#define ESC_HOME     "\x1b[H"       /* カーソルを左上へ */
#define ESC_RESET    "\x1b[0m"      /* 色リセット */
#define ESC_HIDE_CUR "\x1b[?25l"    /* カーソル非表示 */
#define ESC_SHOW_CUR "\x1b[?25h"    /* カーソル表示 */

/* 色定義 (文字色) */
#define COL_CYAN     "\x1b[36m"     /* I: 水色 */
#define COL_YELLOW   "\x1b[33m"     /* O: 黄色 */
#define COL_GREEN    "\x1b[32m"     /* S: 緑 */
#define COL_RED      "\x1b[31m"     /* Z: 赤 */
#define COL_BLUE     "\x1b[34m"     /* J: 青 */
#define COL_MAGENTA  "\x1b[35m"     /* L: 紫 */
#define COL_WHITE    "\x1b[37m"     /* T: 白 */
#define COL_WALL     "\x1b[37m"     /* 壁: 白 */

/* -------------------------------------------------------------------
 * グローバル変数
 * ------------------------------------------------------------------- */
/* * fieldの値の意味:
 * 0: 空白
 * 1: 壁
 * 2〜8: 固定されたミノの色ID (ミノタイプ + 2) 
 */
char field[FIELD_HEIGHT][FIELD_WIDTH];
char displayBuffer[FIELD_HEIGHT][FIELD_WIDTH];

/* ミノの定義 */
enum {
    MINO_TYPE_I,
    MINO_TYPE_O,
    MINO_TYPE_S,
    MINO_TYPE_Z,
    MINO_TYPE_J,
    MINO_TYPE_L,
    MINO_TYPE_T,
    MINO_TYPE_MAX
};

/* 色コードの配列 (MINO_TYPEの順に対応) */
const char* minoColors[MINO_TYPE_MAX] = {
    COL_CYAN,    /* I */
    COL_YELLOW,  /* O */
    COL_GREEN,   /* S */
    COL_RED,     /* Z */
    COL_BLUE,    /* J */
    COL_MAGENTA, /* L */
    COL_WHITE    /* T */
};

enum {
    MINO_ANGLE_0,
    MINO_ANGLE_90,
    MINO_ANGLE_180,
    MINO_ANGLE_270,
    MINO_ANGLE_MAX
};

/* ミノ形状データ (0/1) */
char minoShapes[MINO_TYPE_MAX][MINO_ANGLE_MAX][MINO_HEIGHT][MINO_WIDTH] = {
    /* MINO_TYPE_I */
    {
        { {0,1,0,0}, {0,1,0,0}, {0,1,0,0}, {0,1,0,0} },
        { {0,0,0,0}, {0,0,0,0}, {1,1,1,1}, {0,0,0,0} },
        { {0,0,1,0}, {0,0,1,0}, {0,0,1,0}, {0,0,1,0} },
        { {0,0,0,0}, {1,1,1,1}, {0,0,0,0}, {0,0,0,0} }
    },
    /* MINO_TYPE_O */
    {
        { {0,0,0,0}, {0,1,1,0}, {0,1,1,0}, {0,0,0,0} },
        { {0,0,0,0}, {0,1,1,0}, {0,1,1,0}, {0,0,0,0} },
        { {0,0,0,0}, {0,1,1,0}, {0,1,1,0}, {0,0,0,0} },
        { {0,0,0,0}, {0,1,1,0}, {0,1,1,0}, {0,0,0,0} }
    },
    /* MINO_TYPE_S */
    {
        { {0,0,0,0}, {0,1,1,0}, {1,1,0,0}, {0,0,0,0} },
        { {0,1,0,0}, {0,1,1,0}, {0,0,1,0}, {0,0,0,0} },
        { {0,0,0,0}, {0,0,1,1}, {0,1,1,0}, {0,0,0,0} },
        { {0,0,0,0}, {0,1,0,0}, {0,1,1,0}, {0,0,1,0} }
    },
    /* MINO_TYPE_Z */
    {
        { {0,0,0,0}, {1,1,0,0}, {0,1,1,0}, {0,0,0,0} },
        { {0,0,1,0}, {0,1,1,0}, {0,1,0,0}, {0,0,0,0} },
        { {0,0,0,0}, {0,1,1,0}, {0,0,1,1}, {0,0,0,0} },
        { {0,0,0,0}, {0,0,1,0}, {0,1,1,0}, {0,1,0,0} }
    },
    /* MINO_TYPE_J */
    {
        { {0,0,1,0}, {0,0,1,0}, {0,1,1,0}, {0,0,0,0} },
        { {0,0,0,0}, {0,1,0,0}, {0,1,1,1}, {0,0,0,0} },
        { {0,0,0,0}, {0,1,1,0}, {0,1,0,0}, {0,1,0,0} },
        { {0,0,0,0}, {1,1,1,0}, {0,0,1,0}, {0,0,0,0} }
    },
    /* MINO_TYPE_L */
    {
        { {0,1,0,0}, {0,1,0,0}, {0,1,1,0}, {0,0,0,0} },
        { {0,0,0,0}, {0,1,1,1}, {0,1,0,0}, {0,0,0,0} },
        { {0,0,0,0}, {0,1,1,0}, {0,0,1,0}, {0,0,1,0} },
        { {0,0,0,0}, {0,0,1,0}, {1,1,1,0}, {0,0,0,0} }
    },
    /* MINO_TYPE_T */
    {
        { {0,0,0,0}, {1,1,1,0}, {0,1,0,0}, {0,0,0,0} },
        { {0,0,1,0}, {0,1,1,0}, {0,0,1,0}, {0,0,0,0} },
        { {0,0,0,0}, {0,0,1,0}, {1,1,1,0}, {0,0,0,0} },
        { {0,0,0,0}, {0,1,0,0}, {0,1,1,0}, {0,1,0,0} }
    }
};

int minoType = 0, minoAngle = 0;
int minoX = 5, minoY = 0;

/* -------------------------------------------------------------------
 * 関数群
 * ------------------------------------------------------------------- */

/* 画面描画 */
void display() {
    int i, j;
    char cellVal;

    /* 表示バッファ作成 (現在のフィールド状態をコピー) */
    memcpy(displayBuffer, field, sizeof(field));

    /* 操作中のミノを表示バッファに書き込む */
    /* ここでは 2 + minoType を書き込む (2〜8) */
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (minoShapes[minoType][minoAngle][i][j]) {
                displayBuffer[minoY + i][minoX + j] = 2 + minoType;
            }
        }
    }

    /* カーソルを左上に戻す */
    printf(ESC_HOME);

    /* 描画ループ */
    for (i = 0; i < FIELD_HEIGHT; i++) {
        for (j = 0; j < FIELD_WIDTH; j++) {
            cellVal = displayBuffer[i][j];

            if (cellVal == 0) {
                printf(" ."); /* 空白 */
            } else if (cellVal == 1) {
                /* 壁 (白) */
                printf("%s[]%s", COL_WALL, ESC_RESET);
            } else if (cellVal >= 2 && cellVal <= 8) {
                /* ミノ (種類に応じた色) */
                /* cellVal - 2 が minoType (0〜6) に対応 */
                printf("%s[]%s", minoColors[cellVal - 2], ESC_RESET);
            } else {
                printf("??"); /* エラー */
            }
        }
        printf("\n");
    }
}

/* 当たり判定 */
int isHit(int _minoX, int _minoY, int _minoType, int _minoAngle) {
    int i, j;
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (minoShapes[_minoType][_minoAngle][i][j] &&
                field[_minoY + i][_minoX + j]) {
                return 1; /* Hit (壁または固定済みブロックに衝突) */
            }
        }
    }
    return 0; /* No Hit */
}

/* ミノのリセット */
void resetMino() {
    minoX = 5;
    minoY = 0;
    minoType = (tick + rand()) % MINO_TYPE_MAX;
    minoAngle = (tick + rand()) % MINO_ANGLE_MAX;
}

/* -------------------------------------------------------------------
 * ゲームタスク
 * ------------------------------------------------------------------- */
void game_task(void) {
    unsigned long next_drop_time;
    int c;
    int i, j;

    /* 初期化 */
    printf(ESC_CLS);      /* 画面クリア */
    printf(ESC_HIDE_CUR); /* カーソル非表示 */
    
    memset(field, 0, sizeof(field));
    
    /* 壁(1)と床(1)の作成 */
    for (i = 0; i < FIELD_HEIGHT; i++) {
        field[i][0] = field[i][FIELD_WIDTH - 1] = 1;
    }
    for (i = 0; i < FIELD_WIDTH; i++) {
        field[FIELD_HEIGHT - 1][i] = 1;
    }

    resetMino();
    display();

    next_drop_time = tick + 1; /* 次に落下する時刻 */

    while (1) {
        /* ----- 1. キー入力処理 ----- */
        c = inbyte(0);

        if (c != -1) {
            switch (c) {
                case 's': /* 下 */
                    if (!isHit(minoX, minoY + 1, minoType, minoAngle)) {
                        minoY++;
                    }
                    break;
                case 'a': /* 左 */
                    if (!isHit(minoX - 1, minoY, minoType, minoAngle)) {
                        minoX--;
                    }
                    break;
                case 'd': /* 右 */
                    if (!isHit(minoX + 1, minoY, minoType, minoAngle)) {
                        minoX++;
                    }
                    break;
                case ' ': /* 回転 */
                    if (!isHit(minoX, minoY, minoType, (minoAngle + 1) % MINO_ANGLE_MAX)) {
                        minoAngle = (minoAngle + 1) % MINO_ANGLE_MAX;
                    }
                    break;
                default:
                    break;
            }
            display();
        }

        /* ----- 2. 時間経過による落下処理 ----- */
        if (tick >= next_drop_time) {
            next_drop_time = tick + 1; /* 速度調整: ここを変更すると速くなる (例: tick + 5 など) */

            if (isHit(minoX, minoY + 1, minoType, minoAngle)) {
                /* 固定処理 */
                for (i = 0; i < MINO_HEIGHT; i++) {
                    for (j = 0; j < MINO_WIDTH; j++) {
                        if (minoShapes[minoType][minoAngle][i][j]) {
                            /* フィールドにミノの種類ID(2〜8)を記録 */
                            field[minoY + i][minoX + j] = 2 + minoType;
                        }
                    }
                }
                
                /* ライン消去判定 */
                for (i = 0; i < FIELD_HEIGHT - 1; i++) {
                    int lineFill = 1;
                    for (j = 1; j < FIELD_WIDTH - 1; j++) {
                        if (field[i][j] == 0) { /* 空白があれば消えない */
                            lineFill = 0;
                            break;
                        }
                    }
                    if (lineFill) {
                        /* 上の行をずらして消去 */
                        int k;
                        for (k = i; k > 0; k--) {
                            memcpy(field[k], field[k - 1], FIELD_WIDTH);
                        }
                        /* 一番上は空(壁付き)に */
                        memset(field[0], 0, FIELD_WIDTH);
                        field[0][0] = field[0][FIELD_WIDTH-1] = 1;
                    }
                }
                
                resetMino();
                
                /* 生成直後のゲームオーバー判定 */
                if (isHit(minoX, minoY, minoType, minoAngle)) {
                    printf("GAME OVER\n");
                    
                    /* 盤面リセット */
                    memset(field, 0, sizeof(field));
                    for (i = 0; i < FIELD_HEIGHT; i++) {
                        field[i][0] = field[i][FIELD_WIDTH - 1] = 1;
                    }
                    for (i = 0; i < FIELD_WIDTH; i++) {
                        field[FIELD_HEIGHT - 1][i] = 1;
                    }
                }
                
            } else {
                minoY++;
            }
            display();
        }

        /* ----- 3. CPU譲渡 ----- */
        skipmt();
    }
}

/* -------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------- */
int main(void) {
    init_kernel();
    set_task(game_task);
    begin_sch();
    return 0;
}
