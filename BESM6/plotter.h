//
// Generic plotter for BESM-6.
//
// Copyright (c) 2024 Serge Vakulenko
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// Canonical source: /home/azizz/Yandex.Disk/besm6-new/dubna/plotter.h
// (MIT, Copyright (c) 2024 Serge Vakulenko).  Verified there by the golden
// tests expect_calcomp_output.bin / expect_tektronix_output.bin /
// expect_watanabe_output.txt.  This is an ANALOGUE of the Calcomp / Tektronix /
// Watanabe plotter back end, NOT the proven БЕНСОН or НИТИ wire protocol:
// the bytes fed in from dispak come from an analogue decoder in extra.c.
// Only "memory.h" was dropped here (unused in this header), so the copy
// compiles without the rest of the Dubna machine.
//
#ifndef DUBNA_PLOTTER_H
#define DUBNA_PLOTTER_H

#include <functional>
#include <string>

class Plotter {
private:
    std::string watanabe;
    std::string calcomp;
    std::string tektronix;

    // Page number starts from 1 and is incremented for each next page.
    // By default it's zero which means page numbers are disabled (single page).
    unsigned page_number{};

public:
    // Save all data files.
    void finish(bool keep_temporary_files);

    // Send one byte to Watanabe WX4675 plotter.
    void watanabe_putch(char ch) { watanabe += ch; }

    // Send one byte to Calcomp plotter.
    void calcomp_putch(char ch) { calcomp += ch; }

    // Send one byte to Tektronix plotter.
    void tektronix_putch(char ch) { tektronix += ch; }

    // Finish current page and start new one.
    void change_page(bool keep_temporary_files);

private:
    // Save output, if available.
    void save_to_file(std::string filename, const std::string &data);

    // Convert output to SVG format.
    void watanabe_convert_svg(std::string filename);
    void calcomp_convert_svg(std::string filename);
    void tektronix_convert_svg(std::string filename);

    // Parse raw file and invoke given routine for each line.
    void watanabe_parse(const std::function<void(char, unsigned, unsigned &)> &func);
    void tektronix_parse(const std::function<void(bool, unsigned, unsigned &)> &func);
    void calcomp_parse(const std::function<void(bool, int, int &)> &func);

    // Append suffix to a file base name.
    // Also optionally append a page number.
    std::string append_suffix(const std::string &basename, const std::string &suffix);
};

#endif // DUBNA_PLOTTER_H
