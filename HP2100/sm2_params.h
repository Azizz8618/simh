/*==============================================================
 * ГЛИССАДА-6 / ПРОЕКТ СМ-2. ПАРАМЕТНЫЙ БЛОК.
 *
 * ВСЕ корректируемые константы эмулятора СМ-2 собраны ЗДЕСЬ.
 * Править остальные .c/.h-файлы нельзя без правки этого блока.
 * Требование пользователя: «изменяемые параметры — в отдельные
 * блоки переменных», docs/corrections.md.
 *
 * Источники:
 *   А. Савватеев, «Обзор архитектуры и системы команд мини-ЭВМ СМ-2М»,
 *     http://www.computer-museum.ru/histussr/sm2mcomm.htm
 *   docs/analysis/sm2-vs-hp2000.md, docs/analysis/sm2-port-plan.md
 *==============================================================*/
#ifndef SM2_PARAMS_H
#define SM2_PARAMS_H

/*--- 1. Модель и память ---------------------------------------*/
#define SM2_MEM_MAX_WORDS   128u*1024u   /* 4 куба по 32К слов */
#define SM2_CUBE_WORDS      32u*1024u
#define SM2_ADDR_BITS       17           /* абсолютный адрес */
#define SM2_WORD_BITS       16
#define SM2_SKIP_STEP       2            /* пропуск: РНК += 2 (не 1) */
#define SM2_LA_MASK         0x1FFFFu     /* 17-разрядный абсолютный адрес */
#define SM2_RB1_SHIFT       9            /* база пользователя: блоки по 512 слов */
#define SM2_RB2_SHIFT       15           /* привилегированная база: кучи по 32К */
#define SM2_INDEX_MASK      0000060u     /* разряды 5,6 = индексная адресация */
#define SM2_INDEX_OFF_MASK  0000777u     /* разряды 7-15: смещение */

/*--- 2. Регистры и слово состояния ----------------------------*/
#define SM2_PSW_BITS        32           /* ССП */
#define SM2_RB1_BITS        8            /* база пользователя */
#define SM2_RB2_BITS        2            /* привилегированная база */
#define SM2_BOUND_BLOCK     512u         /* границы РВГ/РНГ, слова */

/*--- 3. Векторы прерываний ------------------------------------*/
#define SM2_VEC_POWER       4            /* авария питания */
#define SM2_VEC_CONTROL     5            /* схема контроля, УСП */
#define SM2_VEC_SVC         6            /* обращение к супервизору */
#define SM2_VEC_FIRST_DEV   10           /* первое устройство (см. КОДЫ) */
#define SM2_VEC_COUNT       64           /* всего векторов */

/*--- 4. Коды выборки «Сопряжение 2К» --------------------------*/
#define SM2_SC_SPECIAL_LO   0            /* 0..17 служебные */
#define SM2_SC_SPECIAL_HI   017
#define SM2_SC_SVV1_LO      020          /* СВВ1 */
#define SM2_SC_SVV1_HI      037
#define SM2_SC_SVV2_LO      040
#define SM2_SC_SVV2_HI      057
#define SM2_SC_SVV3_LO      060
#define SM2_SC_SVV3_HI      077
#define SM2_SVV_COUNT       3
#define SM2_RIM_MAX_DEV     1512u        /* до 1512 ВУ через РИМ */
#define SM2_RIM_MAX_DIST    3000u        /* до 3 км */

/*--- 5. Сигналы интерфейса 2К (шины К и Т) --------------------*/
#define SM2_BUS_K_LINES     27
#define SM2_BUS_T_LINES     25
#define SM2_DATA_BITS       16
#define SM2_KPDM_MAX_BPS    1400000u     /* 1,4 Мбайт/с */

/*--- 6. Тайминг (мкс, по статье Савватеева) ------------------*/
#define SM2_T_ADD           2.0f         /* ADA/ADB */
#define SM2_T_LOAD          2.0f         /* LDA/LDB */
#define SM2_T_JMP           1.7f
#define SM2_T_COMPARE       2.8f
#define SM2_T_JSB           3.8f
#define SM2_T_ISZ           3.9f
#define SM2_T_FLOAT_ADD     21.9f
#define SM2_T_FLOAT_DIV     35.1f
#define SM2_T_INDIRECT_EXTRA 1.3f        /* косвенная адресация */
#define SM2_T_INDEX_EXTRA   1.0f         /* индекс/автоиндекс */
#define SM2_T_IO_EXTRA      0.8f         /* косвенная адресация ВУ */

/*--- 7. Команды ввода-вывода (биты 0-3 и 5 = 100012) ----------*/
#define SM2_IO_PREFIX       0100012u
#define SM2_IO_OP_SHIFT     4            /* разряды 4, 6-9 */
#define SM2_IO_SC_SHIFT     10           /* разряды 10-15 */

/*--- 8. Контрольные точки этапа 0 -----------------------------*/
#define SM2_BASELINE_CPU    "2116"
#define SM2_BASELINE_MEM    32u*1024u

#endif /* SM2_PARAMS_H */
