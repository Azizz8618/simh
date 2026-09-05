#!/usr/bin/env perl
use strict;
use warnings;

my %func;
open(my $f, '<', '/tmp/base_0677.txt') or die $!;
while(<$f>) {
    if (/^0677\.(\d{4}):/) {
        $func{$1} = $_;
    }
}
close($f);

my %used;
open(my $t, '<', '/home/azizz/Yandex.Disk/simh/BESM6/OSZAGR/tr12.txt') or die $!;
while(<$t>) {
    while (/0677 (\d{4})[LR]/g) {
        $used{$1}++;
    }
}
close($t);

print "Анализ модуля СВЯЗЬ7 (зона 0677):\n";
printf "%-10s | %-10s | %s\n", "Адрес", "Вызовов", "Команда";
print "-" x 60 . "\n";
for my $addr (sort keys %used) {
    if (exists $func{$addr}) {
        chomp $func{$addr};
        my @p = split(/\s+/, $func{$addr});
        printf "0677.%s | %-9d | %s\n", $addr, $used{$addr}, "$p[1] $p[2] $p[3] $p[4]";
    }
}
