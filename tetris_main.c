/* ===================================================================
 * tetris_main.c
 * 機能: 2ポート対戦，差分描画，7種1巡，お邪魔ブロック攻撃
 * 変更: 
 * - 背景黒化，RGBカラー指定への対応
 * - 落下速度調整
 * - お邪魔ブロックせり上がり修正（床を残す）
 * - 勝敗判定時の画面クリアと勝利メッセージ表示
 * =================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mtk_c.h"

/* -------------------------------------------------------------------
 * ファイルポインタの参照 (extern)
 * ------------------------------------------------------------------- */
extern FILE *com0in;
extern FILE *com0out;
extern FILE *com1in;
extern FILE *com1out;

/* -------------------------------------------------------------------
 * 外部関数の宣言
 * ------------------------------------------------------------------- */
extern void init_kernel(void);
extern void set_task(void (*func)());
extern void begin_sch(void);
extern int inbyte(int ch);      /* ノンブロッキング入力 (データがないと-1を返す) */
extern void skipmt(void);       /* CPU時間を自発的に譲る */

/* -------------------------------------------------------------------
 * 定数・マクロ定義
 * ------------------------------------------------------------------- */
#define FIELD_WIDTH  12     /* フィールドの幅 (壁含む) */
#define FIELD_HEIGHT 22     /* フィールドの高さ (床含む) */

#define MINO_WIDTH   4      /* ミノのデータサイズ */
#define MINO_HEIGHT  4

/* 落下スピード */
#define DROP_INTERVAL 1000

/* VT100 エスケープシーケンス (画面制御用) */
#define ESC_CLS      "\x1b[2J"      /* 画面全体クリア */
#define ESC_HOME     "\x1b[H"       /* カーソルをホーム(左上)へ */
#define ESC_RESET    "\x1b[0m"      /* 色属性リセット */
#define ESC_HIDE_CUR "\x1b[?25l"    /* カーソルを隠す */
#define ESC_SHOW_CUR "\x1b[?25h"    /* カーソルを表示 */
#define ESC_CLR_LINE "\x1b[K"       /* カーソル位置から行末まで消去 */

/* 色定義 (文字色) - RGB指定 (TrueColor) */
#define COL_CYAN     "\x1b[38;2;0;255;255m"     /* Iミノ: 水色 */
#define COL_YELLOW   "\x1b[38;2;255;255;0m"     /* Oミノ: 黄色 */
#define COL_PURPLE   "\x1b[38;2;160;32;240m"    /* Tミノ: 紫 */
#define COL_BLUE     "\x1b[38;2;0;0;255m"       /* Jミノ: 青 */
#define COL_ORANGE   "\x1b[38;2;255;165;0m"     /* Lミノ: オレンジ */
#define COL_GREEN    "\x1b[38;2;0;255;0m"       /* Sミノ: 緑 */
#define COL_RED      "\x1b[38;2;255;0;0m"       /* Zミノ: 赤 */
#define COL_WHITE    "\x1b[38;2;255;255;255m"   /* 壁: 白 */
#define COL_GRAY     "\x1b[38;2;128;128;128m"   /* お邪魔ブロック: 灰色 */

/* 背景色定義 */
#define BG_BLACK     "\x1b[40m"                 /* 背景: 黒 */

/* 壁の色として使用するマクロ */
#define COL_WALL     COL_WHITE

/* -------------------------------------------------------------------
 * ゲーム状態を管理する構造体の定義
 * ------------------------------------------------------------------- */
typedef struct {
    int port_id;            /* ポートID (0 or 1) */
    FILE *fp_out;           /* 出力先ファイルポインタ */
    
    /* 盤面データ: 0=空，1=壁，2~8=ミノ，9=お邪魔 */
    char field[FIELD_HEIGHT][FIELD_WIDTH];
    
    /* 描画用バッファ */
    char displayBuffer[FIELD_HEIGHT][FIELD_WIDTH];
    
    /* 差分描画用: 前回の画面状態を保持 */
    char prevBuffer[FIELD_HEIGHT][FIELD_WIDTH];
    int force_refresh;      /* 1のとき画面全体を強制再描画 */

    /* 操作中のミノ情報 */
    int minoType;           /* 種類 */
    int minoAngle;          /* 角度 */
    int minoX;              /* X座標 */
    int minoY;              /* Y座標 */
    
    /* 7種1巡(7-Bag)システム用変数 */
    int bag[7];             /* 補充用バッグ (0-6のミノIDが入る) */
    int bag_index;          /* 現在何個目を取り出しているか */

    /* ゲーム進行管理 */
    unsigned long next_drop_time; /* 次に自然落下する時刻(tick) */
    unsigned int random_seed;     /* (未使用だが将来的な拡張用) */
    
    /* スコア・状況管理 */
    int score;              /* 現在の得点 */
    int lines_cleared;      /* 消したライン総数 */

    /* 対戦用パラメータ */
    int pending_garbage;    /* 相手から送られ，まだせり上がっていないお邪魔段数 */
    int is_gameover;        /* 1ならゲームオーバー状態 */

} TetrisGame;

/* タスク間通信用: お互いのゲーム状態を参照するためのポインタ配列 */
TetrisGame *all_games[2] = {NULL, NULL};

/* -------------------------------------------------------------------
 * グローバル変数 (定数データ)
 * ------------------------------------------------------------------- */

/* ミノの種類定義 */
enum {
    MINO_TYPE_I,
    MINO_TYPE_O,
    MINO_TYPE_S,
    MINO_TYPE_Z,
    MINO_TYPE_J,
    MINO_TYPE_L,
    MINO_TYPE_T,
    MINO_TYPE_GARBAGE, /* お邪魔ブロック */
    MINO_TYPE_MAX
};

/* ミノの色定義 (指定された配色) */
const char* minoColors[MINO_TYPE_MAX] = {
    COL_CYAN,    /* I: 水色 */
    COL_YELLOW,  /* O: 黄色 */
    COL_GREEN,   /* S: 緑 */
    COL_RED,     /* Z: 赤 */
    COL_BLUE,    /* J: 青 */
    COL_ORANGE,  /* L: オレンジ */
    COL_PURPLE,  /* T: 紫 */
    COL_GRAY     /* Garbage: 灰色 */
};

/* 回転角度 */
enum {
    MINO_ANGLE_0,
    MINO_ANGLE_90,
    MINO_ANGLE_180,
    MINO_ANGLE_270,
    MINO_ANGLE_MAX
};

/* ミノの形状データ [種類][角度][y][x] */
char minoShapes[MINO_TYPE_MAX][MINO_ANGLE_MAX][MINO_HEIGHT][MINO_WIDTH] = {
    /* MINO_TYPE_I */
    {
        { {0, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 0, 0} },
        { {0, 0, 0, 0}, {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 0, 0} },
        { {0, 0, 1, 0}, {0, 0, 1, 0}, {0, 0, 1, 0}, {0, 0, 1, 0} },
        { {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 0, 0, 0}, {0, 0, 0, 0} }
    },
    /* MINO_TYPE_O */
    {
        { {0, 0, 0, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 1, 0}, {0, 1, 1, 0}, {0, 0, 0, 0} }
    },
    /* MINO_TYPE_S */
    {
        { {0, 0, 0, 0}, {0, 1, 1, 0}, {1, 1, 0, 0}, {0, 0, 0, 0} },
        { {0, 1, 0, 0}, {0, 1, 1, 0}, {0, 0, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 0, 1, 1}, {0, 1, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 0, 0}, {0, 1, 1, 0}, {0, 0, 1, 0} }
    },
    /* MINO_TYPE_Z */
    {
        { {0, 0, 0, 0}, {1, 1, 0, 0}, {0, 1, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 1, 0}, {0, 1, 1, 0}, {0, 1, 0, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 1, 0}, {0, 0, 1, 1}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 0, 1, 0}, {0, 1, 1, 0}, {0, 1, 0, 0} }
    },
    /* MINO_TYPE_J */
    {
        { {0, 0, 1, 0}, {0, 0, 1, 0}, {0, 1, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 0, 0}, {0, 1, 1, 1}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 1, 0}, {0, 1, 0, 0}, {0, 1, 0, 0} },
        { {0, 0, 0, 0}, {1, 1, 1, 0}, {0, 0, 1, 0}, {0, 0, 0, 0} }
    },
    /* MINO_TYPE_L */
    {
        { {0, 1, 0, 0}, {0, 1, 0, 0}, {0, 1, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 1, 1}, {0, 1, 0, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 1, 0}, {0, 0, 1, 0}, {0, 0, 1, 0} },
        { {0, 0, 0, 0}, {0, 0, 1, 0}, {1, 1, 1, 0}, {0, 0, 0, 0} }
    },
    /* MINO_TYPE_T */
    {
        { {0, 0, 0, 0}, {1, 1, 1, 0}, {0, 1, 0, 0}, {0, 0, 0, 0} },
        { {0, 0, 1, 0}, {0, 1, 1, 0}, {0, 0, 1, 0}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 0, 1, 0}, {0, 1, 1, 1}, {0, 0, 0, 0} },
        { {0, 0, 0, 0}, {0, 1, 0, 0}, {0, 1, 1, 0}, {0, 1, 0, 0} }
    },
    /* MINO_TYPE_GARBAGE (ダミー定義) */
    {
        {{0}}, {{0}}, {{0}}, {{0}}
    }
};

/* -------------------------------------------------------------------
 * 関数群
 * ------------------------------------------------------------------- */

/*
 * display: 画面描画関数
 * 差分描画を行い，通信量を削減して高速化している．
 */
void display(TetrisGame *game) {
    int i, j;
    char cellVal;
    int changes = 0;

    /* 1. 現在の盤面データをバッファにコピー */
    memcpy(game->displayBuffer, game->field, sizeof(game->field));

    /* 2. 操作中のミノをバッファに重ね書き */
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

    /* 3. 情報表示 (スコアなどは毎回更新) */
    fprintf(game->fp_out, "\x1b[1;1H"); /* カーソルを左上へ */
    fprintf(game->fp_out, "SCORE: %-6d  LINES: %-4d  ATK: %d%s", 
            game->score, game->lines_cleared, game->pending_garbage, ESC_CLR_LINE);
    fprintf(game->fp_out, "\n--------------------------------%s", ESC_CLR_LINE);

    /* 4. 盤面の差分描画 */
    int offset_y = 3; /* 盤面の開始行 (ヘッダの分ずらす) */

    for (i = 0; i < FIELD_HEIGHT; i++) {
        for (j = 0; j < FIELD_WIDTH; j++) {
            cellVal = game->displayBuffer[i][j];

            if (game->force_refresh || cellVal != game->prevBuffer[i][j]) {
                fprintf(game->fp_out, "\x1b[%d;%dH", i + offset_y, j * 2 + 1);

                if (cellVal == 0) {
                    fprintf(game->fp_out, "%s .%s", BG_BLACK, ESC_RESET);
                } else if (cellVal == 1) {
                    fprintf(game->fp_out, "%s%s[]%s", BG_BLACK, COL_WALL, ESC_RESET);
                } else if (cellVal >= 2 && cellVal <= 9) {
                    fprintf(game->fp_out, "%s%s[]%s", BG_BLACK, minoColors[cellVal - 2], ESC_RESET);
                } else {
                    fprintf(game->fp_out, "??");
                }

                game->prevBuffer[i][j] = cellVal;
                changes++;
            }
        }
    }
    
    game->force_refresh = 0; /* フラグを下ろす */
    if (changes > 0) fflush(game->fp_out);
}

/*
 * isHit: 当たり判定
 */
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

/*
 * fillBag: 7種1巡システム用バッグの補充とシャッフル
 */
void fillBag(TetrisGame *game) {
    int i, j, temp;
    for (i = 0; i < 7; i++) game->bag[i] = i;
    for (i = 6; i > 0; i--) {
        j = (tick + rand()) % (i + 1); 
        temp = game->bag[i]; game->bag[i] = game->bag[j]; game->bag[j] = temp;
    }
    game->bag_index = 0;
}

/*
 * resetMino: 次のミノをセットする
 */
void resetMino(TetrisGame *game) {
    game->minoX = 5;
    game->minoY = 0;
    if (game->bag_index >= 7) fillBag(game);
    game->minoType = game->bag[game->bag_index];
    game->bag_index++;
    game->minoAngle = (tick + rand()) % MINO_ANGLE_MAX;
}

/*
 * processGarbage: お邪魔ブロックのせり上がり処理
 * 修正: 床(最下段)を保持し、その一つ上までをせり上げる
 */
int processGarbage(TetrisGame *game) {
    int lines = game->pending_garbage;
    if (lines <= 0) return 0;
    if (lines > 4) lines = 4;

    game->pending_garbage -= lines;

    int i, j, k;
    
    /* 1. 上部にはみ出すブロックがないかチェック */
    for (k = 0; k < lines; k++) {
        for (j = 1; j < FIELD_WIDTH - 1; j++) {
            if (game->field[k][j] != 0) return 1; /* 死 */
        }
    }

    /* 2. フィールド全体を上にずらす (床 FIELD_HEIGHT-1 は触らない) */
    /* FIELD_HEIGHT - 1 - lines までループする */
    for (i = 0; i < FIELD_HEIGHT - 1 - lines; i++) {
        memcpy(game->field[i], game->field[i + lines], FIELD_WIDTH);
    }

    /* 3. 下からお邪魔ブロックラインを生成 (床の一つ上まで) */
    for (i = FIELD_HEIGHT - 1 - lines; i < FIELD_HEIGHT - 1; i++) {
        game->field[i][0] = 1;
        game->field[i][FIELD_WIDTH - 1] = 1;
        for (j = 1; j < FIELD_WIDTH - 1; j++) {
            game->field[i][j] = 2 + MINO_TYPE_GARBAGE; 
        }
        int hole = 1 + (tick + rand() + i) % (FIELD_WIDTH - 2);
        game->field[i][hole] = 0;
    }

    game->force_refresh = 1;
    return 0; /* 生存 */
}

/*
 * show_gameover_message: 敗北時の画面表示
 */
void show_gameover_message(TetrisGame *game) {
    fprintf(game->fp_out, ESC_CLS); /* 画面クリア */
    fprintf(game->fp_out, ESC_HOME);
    fprintf(game->fp_out, "%s\n", COL_RED);
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "         GAME OVER          \n");
    fprintf(game->fp_out, "          YOU LOSE          \n");
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "%s\n", ESC_RESET);
    fprintf(game->fp_out, "Final Score: %d\n", game->score);
    fflush(game->fp_out);
}

/*
 * show_victory_message: 勝利時の画面表示
 */
void show_victory_message(TetrisGame *game) {
    fprintf(game->fp_out, ESC_CLS); /* 画面クリア */
    fprintf(game->fp_out, ESC_HOME);
    fprintf(game->fp_out, "%s\n", COL_CYAN);
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "      CONGRATULATIONS!      \n");
    fprintf(game->fp_out, "          YOU WIN!          \n");
    fprintf(game->fp_out, "============================\n");
    fprintf(game->fp_out, "%s\n", ESC_RESET);
    fprintf(game->fp_out, "Score: %d\n", game->score);
    fflush(game->fp_out);
}

/* -------------------------------------------------------------------
 * 汎用ゲームロジック (run_tetris)
 * ------------------------------------------------------------------- */
void run_tetris(TetrisGame *game) {
    int c;
    int i, j;
    
    game->score = 0;
    game->lines_cleared = 0;
    game->pending_garbage = 0;
    game->is_gameover = 0;
    game->force_refresh = 1;
    memset(game->prevBuffer, 0, sizeof(game->prevBuffer));
    game->bag_index = 7;

    /* 画面クリアとカーソル非表示 */
    fprintf(game->fp_out, ESC_CLS);      
    fprintf(game->fp_out, ESC_HIDE_CUR); 
    
    /* フィールド初期化 */
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

    /* --- メインループ --- */
    while (1) {
        
        /* 0. 相手の状態チェック (勝利判定) */
        int opponent_id = (game->port_id == 0) ? 1 : 0;
        if (all_games[opponent_id] != NULL && all_games[opponent_id]->is_gameover) {
            /* 相手が死んでいる -> 勝利 */
            show_victory_message(game);
            return; /* ゲームループを抜ける */
        }

        /* 1. キー入力処理 */
        c = inbyte(game->port_id);
        if (c != -1) {
            switch (c) {
                case 's':
                    if (!isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                        game->minoY++;
                        game->next_drop_time  = tick + DROP_INTERVAL;
                    }
                    break;
                case 'a':
                    if (!isHit(game, game->minoX - 1, game->minoY, game->minoType, game->minoAngle)) {
                        game->minoX--;
                    }
                    break;
                case 'd':
                    if (!isHit(game, game->minoX + 1, game->minoY, game->minoType, game->minoAngle)) {
                        game->minoX++;
                    }
                    break;
                case ' ':
                    if (!isHit(game, game->minoX, game->minoY, game->minoType, (game->minoAngle + 1) % MINO_ANGLE_MAX)) {
                        game->minoAngle = (game->minoAngle + 1) % MINO_ANGLE_MAX;
                    }
                    break;
                case 'q':
                    fprintf(game->fp_out, "%sQuit.\n", ESC_SHOW_CUR);
                    return;
            }
            display(game);
        }

        /* 2. 時間経過による自然落下処理 */
        if (tick >= game->next_drop_time) {
            game->next_drop_time = tick + DROP_INTERVAL;

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
                
                /* ライン消去 & 攻撃 */
                {
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
                        game->lines_cleared += lines_this_turn;
                        game->force_refresh = 1;
                        
                        int attack = 0;
                        if (lines_this_turn == 2) attack = 1;
                        if (lines_this_turn == 3) attack = 2;
                        if (lines_this_turn == 4) attack = 4;
                        
                        if (attack > 0) {
                            if (all_games[opponent_id] != NULL && !all_games[opponent_id]->is_gameover) {
                                all_games[opponent_id]->pending_garbage += attack;
                            }
                        }
                        switch (lines_this_turn) {
                            case 1: game->score += 100; break;
                            case 2: game->score += 300; break;
                            case 3: game->score += 500; break;
                            case 4: game->score += 800; break;
                        }
                    }
                }
                
                /* せり上がり処理 */
                if (processGarbage(game)) {
                    /* せり上がり死亡 */
                    game->is_gameover = 1;
                    show_gameover_message(game);
                    return;
                }

                resetMino(game);
                
                /* 窒息判定 */
                if (isHit(game, game->minoX, game->minoY, game->minoType, game->minoAngle)) {
                    /* 窒息死亡 */
                    game->is_gameover = 1;
                    show_gameover_message(game);
                    return; 
                }
                
            } else {
                game->minoY++;
            }
            display(game);
        }

        /* 3. CPU譲渡 */
        skipmt();
    }
}

/* -------------------------------------------------------------------
 * タスク群
 * ------------------------------------------------------------------- */
void task1(void) {
    TetrisGame game1;
    game1.port_id = 0;
    game1.fp_out = com0out;
    all_games[0] = &game1; 
    
    while(1) {
        run_tetris(&game1);
        /* ゲーム終了後しばらく待機 */
        int i;
        for(i=0; i<200000; i++){ 
            if (i % 100 == 0) skipmt(); 
        }
    }
}

void task2(void) {
    TetrisGame game2;
    game2.port_id = 1;
    game2.fp_out = com1out;
    all_games[1] = &game2;
    
    while(1) {
        run_tetris(&game2);
        int i;
        for(i=0; i<200000; i++){ 
            if (i % 100 == 0) skipmt(); 
        }
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