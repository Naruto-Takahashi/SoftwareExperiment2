/* ===================================================================
 * test3.c (修正版)
 * テーマ3: 応用（2ポートI/Oテスト & マルチタスク動作確認）
 * fscanf を廃止し，fgets + sscanf を使用する安全な実装に変更
 * =================================================================== */
#include <stdio.h>
#include <string.h> /* sscanf等のため */
#include "mtk_c.h"

/* -------------------------------------------------------------------
 * 外部関数の宣言
 * ------------------------------------------------------------------- */
extern void init_kernel(void);
extern void set_task(void (*func)());
extern void begin_sch(void);

/* -------------------------------------------------------------------
 * 大域変数の宣言 (extern)
 * ※ 実体は mtk_c.c にあるため，ここでは extern をつけて参照する
 * ------------------------------------------------------------------- */
extern FILE *com0in;
extern FILE *com0out;
extern FILE *com1in;
extern FILE *com1out;

/* -------------------------------------------------------------------
 * ユーザタスク定義
 * ------------------------------------------------------------------- */

/* タスク1: Port0 (UART1) 担当 */
void task1(void)
{
    char line_buf[64]; /* 入力バッファ（スタックを使いすぎないサイズに） */
    int num;
    int count = 0;

    /* com0out が正しくオープンされているか確認して出力 */
    if (com0out != NULL) {
        fprintf(com0out, "Task1 (Port0/UART1) Started.\n");
        fprintf(com0out, "Input number on Port0:\n");
        fflush(com0out); /* バッファを掃き出す */
    }

    while (1) {
        /* --- 修正ポイント ---
         * fscanf はスタック消費が激しく，予期せぬブロッキングの原因になるため
         * fgets で1行読み込んでから解析する方式に変更しました．
         * fgets は改行(Enter)まで読み込むと必ずリターンするため，制御が戻ってきます．
         */
        
        /* データ入力待ち (データがない間は read 内で skipmt される) */
        if (com0in != NULL && fgets(line_buf, sizeof(line_buf), com0in) != NULL) {
            
            /* 読み込んだ文字列バッファから数値を抽出 */
            if (sscanf(line_buf, "%d", &num) == 1) {
                /* 数値の読み取りに成功した場合 */
                fprintf(com0out, "Task1: You entered %d. (count=%d)\n", num, ++count);
                fprintf(com0out, "Next input (Port0):\n");
                fflush(com0out);
            } else {
                /* 数値以外が入力された場合や，空エンターの場合は何もしない
                   (次の入力を待つループへ戻る) */
            }
        }
    }
}

/* タスク2: Port1 (UART2) 担当 */
void task2(void)
{
    char line_buf[64]; /* 入力バッファ */
    int num;
    int count = 0;

    /* com1out が正しくオープンされているか確認して出力 */
    if (com1out != NULL) {
        fprintf(com1out, "Task2 (Port1/UART2) Started.\n");
        fprintf(com1out, "Input number on Port1:\n");
        fflush(com1out);
    }

    while (1) {
        /* Task1と同様に fgets + sscanf を使用 */
        if (com1in != NULL && fgets(line_buf, sizeof(line_buf), com1in) != NULL) {
            
            if (sscanf(line_buf, "%d", &num) == 1) {
                fprintf(com1out, "Task2: You entered %d. (count=%d)\n", num, ++count);
                fprintf(com1out, "Next input (Port1):\n");
                fflush(com1out);
            }
        }
    }
}

/* -------------------------------------------------------------------
 * main関数
 * ------------------------------------------------------------------- */
int main(void)
{
    /* 1. カーネルの初期化 */
    init_kernel();

    /* 2. ストリームの割り当て (テーマ3 3.1節) */
    
    /* Port0 (UART1) 用のストリーム確保 */
    com0in  = fdopen(0, "r");
    com0out = fdopen(1, "w");

    /* Port1 (UART2) 用のストリーム確保 */
    com1in  = fdopen(4, "r");
    com1out = fdopen(4, "w");

    /* エラーチェック (メモリ不足等) */
    if (com0in == NULL || com0out == NULL) {
        /* エラー処理が必要ならここに記述 */
    }
    if (com1in == NULL || com1out == NULL) {
        if (com0out) fprintf(com0out, "Warning: Failed to open Port1.\n");
    }

    /* 3. ユーザタスクの登録 */
    set_task(task1);
    set_task(task2);

    /* 4. マルチタスクスケジューリングの開始 */
    begin_sch();

    return 0;
}
