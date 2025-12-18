/* ===================================================================
 * tetris_main.c
 * 機能: 2ポート対戦，差分描画，7種1巡，お邪魔ブロック攻撃
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

/* 落下スピード (tick単位) */
#define DROP_INTERVAL 5

/* VT100 エスケープシーケンス (画面制御用) */
#define ESC_CLS      "\x1b[2J"      /* 画面全体クリア */
#define ESC_HOME     "\x1b[H"       /* カーソルをホーム(左上)へ */
#define ESC_RESET    "\x1b[0m"      /* 色属性リセット */
#define ESC_HIDE_CUR "\x1b[?25l"    /* カーソルを隠す */
#define ESC_SHOW_CUR "\x1b[?25h"    /* カーソルを表示 */
#define ESC_CLR_LINE "\x1b[K"       /* カーソル位置から行末まで消去 */

/* 色定義 (文字色) */
#define COL_CYAN     "\x1b[36m"     /* Iミノ */
#define COL_YELLOW   "\x1b[33m"     /* Lミノ */
#define COL_GREEN    "\x1b[32m"     /* Sミノ */
#define COL_RED      "\x1b[31m"     /* Zミノ */
#define COL_BLUE     "\x1b[34m"     /* Jミノ */
#define COL_MAGENTA  "\x1b[35m"     /* Tミノ */
#define COL_WHITE    "\x1b[37m"     /* Oミノ */
#define COL_WALL     "\x1b[37m"     /* 壁 */
#define COL_GRAY     "\x1b[37m"     /* お邪魔ブロック (白で代用) */

/* -------------------------------------------------------------------
 * ゲーム状態を管理する構造体の定義
 * この構造体をタスクごとに持つことで，独立したゲーム進行を可能にする
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
/* all_games[0] には Task1(Port0)，all_games[1] には Task2(Port1) の構造体が入る */
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
    COL_WHITE,   /* O: 白 */
    COL_GREEN,   /* S: 緑 */
    COL_RED,     /* Z: 赤 */
    COL_BLUE,    /* J: 青 */
    COL_YELLOW,  /* L: 黄色 */
    COL_MAGENTA, /* T: 紫 */
    COL_GRAY     /* Garbage */
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
                    /* fieldの値は (2 + ミノタイプ) */
                    game->displayBuffer[game->minoY + i][game->minoX + j] = 2 + game->minoType;
                }
            }
        }
    }

    /* 3. 情報表示 (スコアなどは毎回更新) */
    fprintf(game->fp_out, "\x1b[1;1H"); /* カーソルを左上へ */
    /* SCORE: 得点, LINES: 消去数, ATK: 自分が受けている未処理のお邪魔数 */
    fprintf(game->fp_out, "SCORE: %-6d  LINES: %-4d  ATK: %d%s", 
            game->score, game->lines_cleared, game->pending_garbage, ESC_CLR_LINE);
    fprintf(game->fp_out, "\n--------------------------------%s", ESC_CLR_LINE);

    /* 4. 盤面の差分描画 */
    int offset_y = 3; /* 盤面の開始行 (ヘッダの分ずらす) */

    for (i = 0; i < FIELD_HEIGHT; i++) {
        for (j = 0; j < FIELD_WIDTH; j++) {
            cellVal = game->displayBuffer[i][j];

            /* 強制再描画フラグが立っているか，前回の内容と異なる場合のみ描画 */
            if (game->force_refresh || cellVal != game->prevBuffer[i][j]) {
                /* カーソルを該当セルへ移動 */
                fprintf(game->fp_out, "\x1b[%d;%dH", i + offset_y, j * 2 + 1);

                /* セルの内容に応じた描画 */
                if (cellVal == 0) {
                    fprintf(game->fp_out, " .");
                } else if (cellVal == 1) {
                    fprintf(game->fp_out, "%s[]%s", COL_WALL, ESC_RESET);
                } else if (cellVal >= 2 && cellVal <= 9) { /* 9はGARBAGE */
                    fprintf(game->fp_out, "%s[]%s", minoColors[cellVal - 2], ESC_RESET);
                } else {
                    fprintf(game->fp_out, "??");
                }

                /* 描画した内容を記憶し，変更カウンタを増やす */
                game->prevBuffer[i][j] = cellVal;
                changes++;
            }
        }
    }
    
    game->force_refresh = 0; /* フラグを下ろす */
    
    /* 変更があった場合のみバッファをフラッシュして画面に反映 */
    if (changes > 0) fflush(game->fp_out);
}

/*
 * isHit: 当たり判定
 * ミノが壁や他のブロックと重なっているかを判定する．
 * 返り値: 1=重なっている(移動不可), 0=重なっていない(移動可)
 */
int isHit(TetrisGame *game, int _minoX, int _minoY, int _minoType, int _minoAngle) {
    int i, j;
    for (i = 0; i < MINO_HEIGHT; i++) {
        for (j = 0; j < MINO_WIDTH; j++) {
            if (minoShapes[_minoType][_minoAngle][i][j]) {
                int fy = _minoY + i;
                int fx = _minoX + j;
                /* フィールド外へのアクセスチェック */
                if (fy < 0 || fy >= FIELD_HEIGHT || fx < 0 || fx >= FIELD_WIDTH) return 1;
                /* ブロックとの衝突チェック */
                if (game->field[fy][fx]) return 1;
            }
        }
    }
    return 0;
}

/*
 * fillBag: 7種1巡システム用バッグの補充とシャッフル
 * 7種類のミノをバッグに入れ，ランダムに並び替える．
 */
void fillBag(TetrisGame *game) {
    int i, j, temp;
    /* 1. バッグに7種類のミノを入れる */
    for (i = 0; i < 7; i++) game->bag[i] = i;
    
    /* 2. シャッフル (Fisher-Yates) */
    for (i = 6; i > 0; i--) {
        j = (tick + rand()) % (i + 1); 
        temp = game->bag[i]; game->bag[i] = game->bag[j]; game->bag[j] = temp;
    }
    game->bag_index = 0; /* インデックスを先頭に戻す */
}

/*
 * resetMino: 次のミノをセットする
 * バッグシステムを使用し，バッグが空になったら補充する．
 */
void resetMino(TetrisGame *game) {
    game->minoX = 5;
    game->minoY = 0;
    
    /* バッグを使い切っていたら補充 */
    if (game->bag_index >= 7) fillBag(game);
    
    game->minoType = game->bag[game->bag_index];
    game->bag_index++;
    
    /* 角度はランダム */
    game->minoAngle = (tick + rand()) % MINO_ANGLE_MAX;
}

/*
 * processGarbage: お邪魔ブロックのせり上がり処理
 * pending_garbage に溜まった分だけ床をせり上げ，最下段に穴空きラインを追加する．
 * 返り値: 1=せり上がりでブロックが天井を突き抜けた(死亡), 0=生存
 */
int processGarbage(TetrisGame *game) {
    int lines = game->pending_garbage;
    if (lines <= 0) return 0;

    /* 一度にせり上がる上限を4段に制限 (ゲームバランス調整) */
    if (lines > 4) lines = 4;

    game->pending_garbage -= lines;

    int i, j, k;
    
    /* 1. 上部にはみ出すブロックがないかチェック (あるならゲームオーバー) */
    /* 上から lines 分の行にブロックがあればアウト */
    for (k = 0; k < lines; k++) {
        for (j = 1; j < FIELD_WIDTH - 1; j++) {
            if (game->field[k][j] != 0) return 1; /* 死 */
        }
    }

    /* 2. フィールド全体を上にずらす (メモリコピー) */
    /* 下の行を上の行へコピー */
    for (i = 0; i < FIELD_HEIGHT - lines; i++) {
        memcpy(game->field[i], game->field[i + lines], FIELD_WIDTH);
    }

    /* 3. 下からお邪魔ブロックラインを生成 */
    for (i = FIELD_HEIGHT - lines; i < FIELD_HEIGHT; i++) {
        /* 両端は壁 */
        game->field[i][0] = 1;
        game->field[i][FIELD_WIDTH - 1] = 1;
        
        /* 中身はお邪魔ブロック(9)で埋める */
        for (j = 1; j < FIELD_WIDTH - 1; j++) {
            game->field[i][j] = 2 + MINO_TYPE_GARBAGE; 
        }
        
        /* ランダムな位置に1つ穴を空ける */
        int hole = 1 + (tick + rand() + i) % (FIELD_WIDTH - 2);
        game->field[i][hole] = 0;
    }

    game->force_refresh = 1; /* 画面全体がずれるので強制再描画 */
    return 0; /* 生存 */
}

/* -------------------------------------------------------------------
 * 汎用ゲームロジック (run_tetris)
 * Task1, Task2 から共通で呼び出されるメインループ
 * ------------------------------------------------------------------- */
void run_tetris(TetrisGame *game) {
    int c;
    int i, j;
    
    /* 変数・フラグ初期化 */
    game->score = 0;
    game->lines_cleared = 0;
    game->pending_garbage = 0;
    game->is_gameover = 0;
    
    game->force_refresh = 1; /* 初回は全描画 */
    memset(game->prevBuffer, 0, sizeof(game->prevBuffer));
    
    game->bag_index = 7; /* これにより初回 resetMino でバッグ生成が走る */

    /* 画面クリアとカーソル非表示 */
    fprintf(game->fp_out, ESC_CLS);      
    fprintf(game->fp_out, ESC_HIDE_CUR); 
    
    /* フィールド初期化 (枠作成) */
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
        /* 1. キー入力処理 */
        c = inbyte(game->port_id);
        if (c != -1) {
            switch (c) {
                case 's': /* 下移動 */
                    if (!isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                        game->minoY++;
                        game->next_drop_time  = tick + DROP_INTERVAL; /* 落下タイマーリセット */
                    }
                    break;
                case 'a': /* 左移動 */
                    if (!isHit(game, game->minoX - 1, game->minoY, game->minoType, game->minoAngle)) {
                        game->minoX--;
                    }
                    break;
                case 'd': /* 右移動 */
                    if (!isHit(game, game->minoX + 1, game->minoY, game->minoType, game->minoAngle)) {
                        game->minoX++;
                    }
                    break;
                case ' ': /* 回転 */
                    if (!isHit(game, game->minoX, game->minoY, game->minoType, (game->minoAngle + 1) % MINO_ANGLE_MAX)) {
                        game->minoAngle = (game->minoAngle + 1) % MINO_ANGLE_MAX;
                    }
                    break;
                case 'q': /* 強制終了 */
                    fprintf(game->fp_out, "%sQuit.\n", ESC_SHOW_CUR);
                    return;
            }
            display(game); /* 操作反映のため描画 */
        }

        /* 2. 時間経過による自然落下処理 */
        if (tick >= game->next_drop_time) {
            game->next_drop_time = tick + DROP_INTERVAL;

            /* 下に移動できるかチェック */
            if (isHit(game, game->minoX, game->minoY + 1, game->minoType, game->minoAngle)) {
                
                /* --- 移動できない場合：固定処理 --- */
                for (i = 0; i < MINO_HEIGHT; i++) {
                    for (j = 0; j < MINO_WIDTH; j++) {
                        if (minoShapes[game->minoType][game->minoAngle][i][j]) {
                            if (game->minoY + i >= 0 && game->minoY + i < FIELD_HEIGHT &&
                                game->minoX + j >= 0 && game->minoX + j < FIELD_WIDTH) {
                                /* フィールドにブロック(種類ID+2)を書き込む */
                                game->field[game->minoY + i][game->minoX + j] = 2 + game->minoType;
                            }
                        }
                    }
                }
                
                /* --- ライン消去判定 & 攻撃処理 --- */
                {
                    int lines_this_turn = 0;
                    for (i = 0; i < FIELD_HEIGHT - 1; i++) {
                        int lineFill = 1;
                        /* 行が埋まっているか確認 */
                        for (j = 1; j < FIELD_WIDTH - 1; j++) {
                            if (game->field[i][j] == 0) { lineFill = 0; break; }
                        }
                        /* 埋まっていたら消去 */
                        if (lineFill) {
                            int k;
                            /* 上の行をずらして埋める */
                            for (k = i; k > 0; k--) memcpy(game->field[k], game->field[k - 1], FIELD_WIDTH);
                            /* 最上段をクリア */
                            memset(game->field[0], 0, FIELD_WIDTH);
                            game->field[0][0] = game->field[0][FIELD_WIDTH-1] = 1;
                            lines_this_turn++;
                        }
                    }

                    if (lines_this_turn > 0) {
                        game->lines_cleared += lines_this_turn;
                        game->force_refresh = 1; /* 画面更新 */
                        
                        /* 攻撃処理: 相手にお邪魔ブロックを送る */
                        int attack = 0;
                        if (lines_this_turn == 2) attack = 1;      /* ダブル -> 1段 */
                        if (lines_this_turn == 3) attack = 2;      /* トリプル -> 2段 */
                        if (lines_this_turn == 4) attack = 4;      /* テトリス -> 4段 */
                        
                        if (attack > 0) {
                            /* 相手のIDを特定 (0なら1，1なら0) */
                            int opponent_id = (game->port_id == 0) ? 1 : 0;
                            /* 相手が存在し，かつゲームオーバーでなければ攻撃を送る */
                            if (all_games[opponent_id] != NULL && !all_games[opponent_id]->is_gameover) {
                                all_games[opponent_id]->pending_garbage += attack;
                            }
                        }

                        /* スコア加算 */
                        switch (lines_this_turn) {
                            case 1: game->score += 100; break;
                            case 2: game->score += 300; break;
                            case 3: game->score += 500; break;
                            case 4: game->score += 800; break;
                        }
                    }
                }
                
                /* --- お邪魔ブロックのせり上がり処理 (ブロック固定後に行う) --- */
                if (processGarbage(game)) {
                    /* せり上がりでブロックが天井を突き抜けた場合 */
                    fprintf(game->fp_out, "\n%sGAME OVER (Garbage)\n", COL_RED);
                    game->is_gameover = 1;
                    return;
                }

                /* 新しいミノを出現させる */
                resetMino(game);
                
                /* --- 出現直後のゲームオーバー判定 (窒息) --- */
                if (isHit(game, game->minoX, game->minoY, game->minoType, game->minoAngle)) {
                    fprintf(game->fp_out, "\n%sGAME OVER\n", COL_RED);
                    fprintf(game->fp_out, "Final Score: %d%s\n", game->score, ESC_RESET);
                    game->is_gameover = 1;
                    return; 
                }
                
            } else {
                /* 移動可能なら一段下げる */
                game->minoY++;
            }
            display(game);
        }

        /* 3. CPU譲渡 (協調的マルチタスク) */
        skipmt();
    }
}

/* -------------------------------------------------------------------
 * タスク群
 * ------------------------------------------------------------------- */

/* Task 1: Port 0 (UART1) 用 */
void task1(void) {
    TetrisGame game1;
    game1.port_id = 0;
    game1.fp_out = com0out;
    all_games[0] = &game1; /* 自分のポインタを登録 (攻撃受信用) */
    
    while(1) {
        run_tetris(&game1);
        
        /* ゲームオーバー後の待機 (CPUを譲りながら待つ) */
        int i;
        for(i=0; i<100000; i++){ 
            if (i % 100 == 0) skipmt(); 
        }
    }
}

/* Task 2: Port 1 (UART2) 用 */
void task2(void) {
    TetrisGame game2;
    game2.port_id = 1;
    game2.fp_out = com1out;
    all_games[1] = &game2; /* 自分のポインタを登録 */
    
    while(1) {
        run_tetris(&game2);
        
        int i;
        for(i=0; i<100000; i++){ 
            if (i % 100 == 0) skipmt(); 
        }
    }
}

/* -------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------- */
int main(void) {
    init_kernel(); /* カーネル初期化 */

    /* ファイルディスクリプタの割り当て */
    com0in  = fdopen(0, "r");
    com0out = fdopen(1, "w");
    com1in  = fdopen(4, "r");
    com1out = fdopen(4, "w");

    /* タスク登録 */
    set_task(task1);
    set_task(task2);

    /* マルチタスク開始 (ここからは戻らない) */
    begin_sch();
    return 0;
}