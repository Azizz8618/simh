    /* Flags are at flags_addr (addr9) through flags_addr+017 */
    if (addr == 0114) {
        /* FIXME: address 14 is a breakdown address.
           The Ps means we store the flags here. */
        int i;
        /*
         * [0] = key value
         * [1..017] = columns A-R, 13 pairs
         *   (v16: upper bits of column B are high byte of key value)
         */
        for (i = 0; i < 80; i++) {
            core[flags_addr + i / 16] =
                (core[flags_addr + i / 16] & ~(0777LL << (3 * (5 - i % 6))))
                | (((uint64) keymap[i]) << (3 * (5 - i % 6)));
        }
        if (DEBUG_LEV_CONF) {
            besm6_log ("\nCONF keymap after processing:\n");
            for (i = 0; i < 16; i++) {
                besm6_log (" row %2d: %02x %02x %02x %02x %02x\n",
                    i, keymap[i*5], keymap[i*5+1], keymap[i*5+2],
                    keymap[i*5+3], keymap[i*5+4]);
            }
        }
    } else {