/*==============================================================
 * ГЛИССАДА-6 / СМ-2. КОНТРОЛЛЕР ИНТЕРФЕЙСА «СОПРЯЖЕНИЕ 2К».
 *
 * Упрощённая модель линии 2К: регистр данных, флаг готовности
 * и прерывание по-vector. Регистрация в sim_devices() producится
 * в hp2100_sys.c. Адрес выбирается командой SET SM2K SC=n.
 *==============================================================*/
#include "hp2100_defs.h"
#include "hp2100_io.h"
#include "sm2_params.h"

static t_stat sm2k_reset (DEVICE *dptr);

static uint32 sm2k_data = 0;                    /* регистр данных линии */
static t_bool sm2k_flag = CLEAR;                /* флаг готовности */

static SIGNALS_VALUE sm2k_interface (const DIB *dptr, INBOUND_SET signals, HP_WORD value);

static DIB sm2k_dib = {
    &sm2k_interface,                                       /* заполняется в initialize_io */
    SM2_SC_SVV1_LO,                             /* код выборки по умолчанию */
    0,
    "SM-2 2K line controller",
    NULL
    };

static REG sm2k_reg [] = {
    { ORDATA (SM2KD, sm2k_data, 16) },
    { ORDATA (SM2KF, sm2k_flag, 1) },
    { 0 }
    };

static UNIT sm2k_unit [] = {
    { UDATA (NULL, 0, 0) }
    };

static MTAB sm2k_mod [] = {
    { MTAB_XDV, 1u, "SC", "SC", &hp_set_dib, &hp_show_dib, (void *) &sm2k_dib },
    { MTAB_XDV | MTAB_NMO, ~1u, "DEVNO", "DEVNO", &hp_set_dib, &hp_show_dib, (void *) &sm2k_dib },
    { 0 }
    };

static SIGNALS_VALUE sm2k_interface (const DIB *dibptr, INBOUND_SET inbound_signals,
                                     HP_WORD inbound_value)
{
INBOUND_SIGNAL signal;
INBOUND_SET    working_set = inbound_signals;
SIGNALS_VALUE  outbound = { ioNONE, 0 };

while (working_set) {
    signal = IONEXTSIG (working_set);

    switch (signal) {

        case ioIOO:                             /* запись в линию (OTA/OTB) */
            sm2k_data = inbound_value;
            sm2k_flag = SET;
            outbound.signals |= ioIRQ | ioFLG;
            break;

        case ioIOI:                             /* чтение из линии (LIA/LIB/MIA/MIB) */
            outbound.value = sm2k_data;
            break;

        case ioSIR:                             /* запрос статуса прерывания */
            if (sm2k_flag == SET)
                outbound.signals |= ioIRQ | ioFLG;
            break;

        case ioIAK:                             /* подтверждение прерывания */
        case ioCLF:                             /* сброс флага */
            sm2k_flag = CLEAR;
            break;

        case ioSTF:                             /* установка флага */
            sm2k_flag = SET;
            break;

        case ioSFS:                             /* пропуск, если флаг установлен */
            if (sm2k_flag == SET)
                outbound.signals |= ioSKF;
            break;

        case ioPOPIO:                           /* начальная установка */
            sm2k_data = 0;
            sm2k_flag = CLEAR;
            break;
        }

    IOCLEARSIG (working_set, signal);
    }

return outbound;
}

DEVICE sm2k_dev = {
    "SM2K",                                     /* имя */
    sm2k_unit,                                  /* устройства */
    sm2k_reg,                                   /* регистры */
    sm2k_mod,                                   /* модификаторы */
    1,                                          /* число устройств */
    8,                                          /* радикс адреса */
    16,                                         /* ширина адреса */
    1,                                          /* приращение */
    8,                                          /* радикс данных */
    16,                                         /* ширина данных */
    NULL, NULL,
    &sm2k_reset,                                /* reset */
    NULL, NULL, NULL,
    &sm2k_dib,                                  /* DIB */
    DEV_DISABLE | DEV_DEBUG,                    /* флаги */
    0, NULL, NULL, NULL, NULL, NULL
    };

static t_stat sm2k_reset (DEVICE *dptr)
{
sm2k_data = 0;
sm2k_flag = CLEAR;
return SCPE_OK;
}
