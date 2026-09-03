# gen_map.awk — извлечение структуры C-файла для карты кода
# Вызов: awk -f gen_map.awk -v src=<basename> < source.c > file.map

BEGIN { nfuncs=0; nvars=0; ndefs=0; ntypedefs=0; in_typedef=0; nlines=0 }

{ gsub(/\r/, ""); nlines++; lines[nlines] = $0 }

END {
    total = nlines

    for (i = 1; i <= total; i++) {
        line = lines[i]
        if (line ~ /^[[:space:]]*$/) continue
        if (line ~ /^[[:space:]]*\/\*/) continue
        if (line ~ /^[[:space:]]*\*/) continue
        if (line ~ /^[[:space:]]*\/\//) continue

        # #define
        if (line ~ /^[[:space:]]*#[[:space:]]*define[[:space:]]+[A-Z_]+/) {
            temp = line
            sub(/^[[:space:]]*#[[:space:]]*define[[:space:]]+/, "", temp)
            split(temp, p, /[[:space:]]/)
            name = p[1]
            val = substr(temp, length(name)+2)
            gsub(/^[[:space:]]+/, "", val)
            gsub(/[[:space:]]+$/, "", val)
            if (val ~ /\\$/) sub(/\\$/, "", val)
            if (length(val) > 35) val = substr(val, 1, 32) "..."
            def_names[ndefs] = name
            def_vals[ndefs] = val
            def_lines[ndefs] = i
            ndefs++
            continue
        }
        if (line ~ /^[[:space:]]*#/) continue

        # typedef struct
        if (line ~ /^[[:space:]]*typedef[[:space:]]+struct/) {
            td_start = i; in_typedef = 1; continue
        }
        if (in_typedef && line ~ /\}[[:space:]]*[A-Za-z_]+[[:space:]]*;/) {
            temp = line
            sub(/^[[:space:]]*\}[[:space:]]*/, "", temp)
            sub(/[[:space:]]*;.*$/, "", temp)
            typedef_names[ntypedefs] = temp
            typedef_starts[ntypedefs] = td_start
            typedef_ends[ntypedefs] = i
            ntypedefs++; in_typedef = 0; continue
        }
        if (in_typedef) continue

        # Только строки с колонки 0
        if (line !~ /^[A-Za-z_]/) continue

        # Определение функции: есть ( и нет ; в конце
        if (line ~ /\(/ && line !~ /;[[:space:]]*$/) {
            is_def = 0
            if (line ~ /\{/) is_def = 1
            else if (i < total) {
                nl = lines[i+1]; gsub(/[[:space:]\r]/, "", nl)
                if (nl == "{") is_def = 1
            }
            if (is_def) {
                paren = index(line, "(")
                if (paren > 0) {
                    bp = substr(line, 1, paren-1)
                    gsub(/^[[:space:]]+/, "", bp)
                    gsub(/[[:space:]]+$/, "", bp)
                    n = split(bp, parts, /[[:space:]]+/)
                    name = parts[n]; gsub(/[*]/, "", name)
                    func_names[nfuncs] = name
                    func_starts[nfuncs] = i
                    # Комментарий над функцией
                    purpose = ""
                    if (i > 1) {
                        prev = lines[i-1]
                        if (prev ~ /^[[:space:]]*\*\/[[:space:]]*$/ && i > 2) {
                            p2 = lines[i-2]
                            if (p2 ~ /^[[:space:]]*\*[[:space:]]+/) {
                                purpose = p2
                                sub(/^[[:space:]]*\*[[:space:]]*/, "", purpose)
                            }
                        } else if (prev ~ /^[[:space:]]*\*[[:space:]]+[A-Za-z0-9]/) {
                            purpose = prev
                            sub(/^[[:space:]]*\*[[:space:]]*/, "", purpose)
                        } else if (prev ~ /^[[:space:]]*\/\*[[:space:]]*[A-Za-z0-9].*\*\//) {
                            purpose = prev
                            sub(/^[[:space:]]*\/\*[[:space:]]*/, "", purpose)
                            sub(/[[:space:]]*\*\/[[:space:]]*$/, "", purpose)
                        } else if (prev ~ /^[[:space:]]*\/\//) {
                            purpose = prev
                            sub(/^[[:space:]]*\/\/[[:space:]]*/, "", purpose)
                        }
                    }
                    func_purposes[nfuncs] = purpose
                    nfuncs++
                }
            }
        } else if (line ~ /;[[:space:]]*$/ && line !~ /\(/) {
            # Глобальная переменная
            temp = line
            sub(/;.*$/, "", temp)
            sub(/=.*$/, "", temp)
            sub(/\[.*$/, "", temp)
            sub(/,.*$/, "", temp)
            gsub(/^[[:space:]]+/, "", temp)
            gsub(/[[:space:]]+$/, "", temp)
            n = split(temp, parts, /[[:space:]]+/)
            name = parts[n]; gsub(/[*]/, "", name)
            if (length(name) > 1 && name !~ /^[0-9]/ && name !~ /^(static|extern|const|unsigned|short|long|signed|volatile|register|void|int|char|FILE|struct|UNIT|DEVICE|jmp_buf|t_stat|t_value|t_addr|t_bool|uint|uint32|int32|TMLN|KMD|CTAB|dks_term_t|DEBTAB|REG|MTAB|t_mtrlnt|t_offset)$/) {
                var_names[nvars] = name
                var_lines[nvars] = i
                nvars++
            }
        }
    }

    # Диапазоны функций
    for (i = 0; i < nfuncs; i++) {
        if (i < nfuncs - 1) func_ends[i] = func_starts[i+1] - 1
        else func_ends[i] = total
    }

    # Вывод
    print "=== " src " — " total " строк ==="
    print "Источник: " src " | Автогенерация: scripts/gen_code_map.sh"
    print "Регенерировать: test " src " -nt .cline/maps/" src ".map"
    print ""

    if (ntypedefs > 0) {
        print "=== СТРУКТУРЫ / TYPEDEF ==="
        print "| Имя | Строки |"
        print "|-----|--------|"
        for (i = 0; i < ntypedefs; i++)
            print "| " typedef_names[i] " | " typedef_starts[i] "-" typedef_ends[i] " |"
        print ""
    }
    if (nvars > 0) {
        print "=== ГЛОБАЛЬНЫЕ ПЕРЕМЕННЫЕ ==="
        print "| Имя | Стр | Назначение |"
        print "|-----|-----|------------|"
        for (i = 0; i < nvars; i++)
            print "| " var_names[i] " | " var_lines[i] " | |"
        print ""
    }
    if (ndefs > 0) {
        print "=== КЛЮЧЕВЫЕ КОНСТАНТЫ / #define ==="
        print "| Имя | Значение | Стр |"
        print "|-----|----------|-----|"
        for (i = 0; i < ndefs; i++) {
            v = def_vals[i]; gsub(/\|/, "\\|", v)
            print "| " def_names[i] " | " v " | " def_lines[i] " |"
        }
        print ""
    }
    if (nfuncs > 0) {
        print "=== ФУНКЦИИ ==="
        print "| Имя | Строки | Назначение |"
        print "|-----|--------|------------|"
        for (i = 0; i < nfuncs; i++) {
            pp = func_purposes[i]; gsub(/\|/, "\\|", pp)
            print "| " func_names[i] " | " func_starts[i] "-" func_ends[i] " | " pp " |"
        }
    }
}
