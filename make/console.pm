#
# hlquery - Search beyond keywords.
# https://www.hlquery.com
#
# Copyright (C) 2021-2026, Carlos F. Ferry <carlos.ferry@gmail.com>
#
# This file is part of hlquery, released under the BSD License version 3.
# You are free to redistribute and/or modify this software
# under the terms of the BSD License.
# For more details, please visit: https://docs.hlquery.com

package make::console;

use v5.26.0;
use strict;
use warnings FATAL => qw(all);

use Exporter qw(import);

# Color constants
use constant {
    RESET   => "\033[0m",
    BOLD    => "\033[1m",
    RED     => "\033[31m",
    GREEN   => "\033[32m",
    YELLOW  => "\033[33m",
    BLUE    => "\033[34m",
    MAGENTA => "\033[35m",
    CYAN    => "\033[36m",
};

our @EXPORT = qw(
    print_header
    print_success
    print_warning
    print_error
    print_info
    print_debug
    print_bold
    print_color
);

sub print_header {
    my ($text) = @_;
    print BOLD . BLUE . "=" x 50 . RESET . "\n";
    print BOLD . BLUE . $text . RESET . "\n";
    print BOLD . BLUE . "=" x 50 . RESET . "\n";
}

sub print_success {
    my ($text) = @_;
    print GREEN . "✓ " . $text . RESET . "\n";
}

sub print_warning {
    my ($text) = @_;
    print YELLOW . "WARNING: " . $text . RESET . "\n";
}

sub print_error {
    my ($text) = @_;
    print RED . "✗ " . $text . RESET . "\n";
}

sub print_info {
    my ($text) = @_;
    print BLUE . "ℹ " . $text . RESET . "\n";
}

sub print_debug {
    my ($text) = @_;
    print MAGENTA . " " . $text . RESET . "\n";
}

sub print_bold {
    my ($text) = @_;
    print BOLD . $text . RESET . "\n";
}

sub print_color {
    my ($text, $color) = @_;
    print $color . $text . RESET . "\n";
}

1;
