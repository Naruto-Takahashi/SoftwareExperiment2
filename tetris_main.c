/* ===================================================================
 * tetris_main.c
 * M68k実験ボード用 テトリス移植版 (Port0 シングルプレイ用)
 * =================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h> /* memset, memcpy用 */
#include "mtk_c.h"

/* -------------------------------------------------------------------
 * 外部関数の宣言
 * ------------------------------------------------------------------- */
extern void init_kernel(void);
extern void set_task(void (*func)());
extern void begin_sch(void);
extern int inbyte(int ch);      /* ノンブロッキング入力 (inchrw.s) */
extern void skipmt(void);       /* CPU譲渡 (mtk_asm.s/csys68k.c) */

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
#define ESC_HIDE_CUR "\x1b[?25l"    /* カーソル非表示 */
#define ESC_SHOW_CUR "\x1b[?25h"    /* カーソル表示 */

/* -------------------------------------------------------------------
 * グローバル変数
 * ------------------------------------------------------------------- */
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

    /* 表示バッファ作成 */
    memcpy(displayBuffer, field, sizeof(field));
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (minoShapes[minoType][minoAngle][i][j]) {
                displayBuffer[minoY + i][minoX + j] = 1;
            }
        }
    }

    /* カーソルを左上に戻す (画面クリアによるチラつき防止) */
    printf(ESC_HOME);

    /* 描画 */
    for (i = 0; i < FIELD_HEIGHT; i++) {
        for (j = 0; j < FIELD_WIDTH; j++) {
            if (displayBuffer[i][j]) {
                printf("[]"); /* ブロック */
            } else {
                printf(" ."); /* 空白 */
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
                return 1; /* Hit */
            }
        }
    }
    return 0; /* No Hit */
}

/* ミノのリセット */
void resetMino() {
    minoX = 5;
    minoY = 0;
    /* rand() は stdlib.h にあるが、乱数種設定(srand)がないと同じ動きになる */
    /* 今回は簡易的に tick を使って少しばらつきを持たせる */
    minoType = (tick + rand()) % MINO_TYPE_MAX;
    minoAngle = (tick + rand()) % MINO_ANGLE_MAX;
}

/* -------------------------------------------------------------------
 * ゲームタスク (メインループ)
 * ------------------------------------------------------------------- */
void game_task(void) {
    unsigned long next_drop_time;
    int c;
    int i, j;

    /* 初期化 */
    printf(ESC_CLS);      /* 最初に一回だけクリア */
    printf(ESC_HIDE_CUR); /* カーソル非表示 */
    
    memset(field, 0, sizeof(field));
    
    /* 壁と床の作成 */
    for (i = 0; i < FIELD_HEIGHT; i++) {
        field[i][0] = field[i][FIELD_WIDTH - 1] = 1;
    }
    for (i = 0; i < FIELD_WIDTH; i++) {
        field[FIELD_HEIGHT - 1][i] = 1;
    }

    resetMino();
    display();

    next_drop_time = tick + 1; /* 次に落下する時刻 (tick単位) */

    while (1) {
        /* ----- 1. キー入力処理 (ノンブロッキング) ----- */
        /* Port0 (UART1) からの直接入力をチェック */
        c = inbyte(0);

        if (c != -1) {
            /* キー入力があった場合 */
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
                case ' ': /* 回転 (Space) */
                    if (!isHit(minoX, minoY, minoType, (minoAngle + 1) % MINO_ANGLE_MAX)) {
                        minoAngle = (minoAngle + 1) % MINO_ANGLE_MAX;
                    }
                    break;
                default:
                    break;
            }
            display(); /* 入力があったらすぐ描画更新 */
        }

        /* ----- 2. 時間経過による落下処理 ----- */
        /* 現在の tick が設定時刻を過ぎているかチェック */
        if (tick >= next_drop_time) {
            /* 次の落下時刻を設定 (スピード調整はここで行う) */
            /* 現在のタイマ設定(1秒)だと '1' で1秒ごとの落下 */
            /* もっと速くしたい場合は mtk_asm.s の init_timer 値を変更する必要あり */
            next_drop_time = tick + 1; 

            if (isHit(minoX, minoY + 1, minoType, minoAngle)) {
                /* 固定処理 */
                for (i = 0; i < MINO_HEIGHT; i++) {
                    for (j = 0; j < MINO_WIDTH; j++) {
                        if (minoShapes[minoType][minoAngle][i][j]) {
                            field[minoY + i][minoX + j] = 1;
                        }
                    }
                }
                
                /* ライン消去判定 */
                for (i = 0; i < FIELD_HEIGHT - 1; i++) {
                    int lineFill = 1;
                    for (j = 1; j < FIELD_WIDTH - 1; j++) {
                        if (!field[i][j]) {
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
                        /* 一番上は空に */
                        memset(field[0], 0, FIELD_WIDTH);
                        field[0][0] = field[0][FIELD_WIDTH-1] = 1;
                    }
                }
                
                resetMino();
                
                /* 生成直後に当たっていたらゲームオーバー (簡易リセット) */
                if (isHit(minoX, minoY, minoType, minoAngle)) {
                    printf("GAME OVER\n");
                    /* ここでループを抜けるか、盤面リセットなどの処理を入れる */
                    memset(field, 0, sizeof(field));
                    for (i = 0; i < FIELD_HEIGHT; i++) {
                        field[i][0] = field[i][FIELD_WIDTH - 1] = 1;
                    }
                    for (i = 0; i < FIELD_WIDTH; i++) {
                        field[FIELD_HEIGHT - 1][i] = 1;
                    }
                }
                
            } else {
                minoY++; /* 落下 */
            }
            
            display(); /* 時間経過更新 */
        }

        /* ----- 3. CPU譲渡 ----- */
        /* 入力がなくてもループを回し続けるため、他タスクのためにCPUを放す */
        skipmt();
    }
}

/* -------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------- */
int main(void) {
    init_kernel();

    /* ゲームタスクの登録 */
    set_task(game_task);

    /* 開始 */
    begin_sch();

    return 0;
}