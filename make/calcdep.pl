#!/usr/bin/env perl
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

use v5.26.0;
use strict;
use warnings FATAL => qw(all);

use File::Basename qw(basename dirname);
use File::Spec::Functions qw(abs2rel catdir catfile);
use File::Path qw(make_path);
use Cwd qw(abs_path);

my $root = abs_path(dirname(dirname $0));
my $src_dir = catdir($root, 'src');
my $include_dir = catdir($root, 'include');
my $build_include_dir = catdir($root, 'build', 'include');
my $obj_dir = catdir($root, 'obj');

print "HLManager Dependency Calculator\n\n";

# Walk src/ and collect every C++ translation unit that needs a .d file.
my @source_files;
find_source_files($src_dir, \@source_files);

print "Found " . scalar(@source_files) . " source files:\n";
foreach my $file (@source_files) {
    print "  $file\n";
}
print "\n";

# Parse includes and emit one make-compatible dependency file per source file.
foreach my $source_file (@source_files) {
    calculate_dependencies($source_file);
}

sub find_source_files {
    my ($dir, $files_ref) = @_;

    opendir my $dh, $dir or die "Cannot open directory $dir: $!";
    while (my $entry = readdir $dh) {
        next if $entry =~ /^\./;

        my $path = catfile($dir, $entry);
        if (-d $path) {
            # Recurse through nested source directories so obj/ mirrors src/.
            find_source_files($path, $files_ref);
        } elsif ($entry =~ /\.(cpp|cxx|cc|c\+\+)$/) {
            push @$files_ref, $path;
        }
    }
    closedir $dh;
}

sub calculate_dependencies {
    my ($source_file) = @_;

    return unless -f $source_file;

    my $base_name = basename($source_file, qw(.cpp .cxx .cc .c++));

    # Convert src/foo/bar.cpp into obj/foo/bar.d.
    my $rel = abs2rel($source_file, $root);
    $rel =~ s/^src\///;
    my $dep_file = catfile($obj_dir, $rel);
    $dep_file =~ s/\.(cpp|cxx|cc|c\+\+)$/.d/;

    print "Calculating dependencies for $source_file\n";

    open my $in, '<', $source_file or die "Cannot open $source_file: $!";
    my %includes;
    my %system_includes;

    while (<$in>) {
        if (/^\s*#\s*include\s+["<]([^">]+)[">]/) {
            my $include = $1;
            if (/^\s*#\s*include\s+"([^"]+)"/) {
                # Track quoted includes because they are project headers.
                $includes{$include} = 1;
            } else {
                # Keep angle-bracket includes out of the .d file but print them for visibility.
                $system_includes{$include} = 1;
            }
        }
    }
    close $in;

    # Ensure the matching obj/ subdirectory exists before writing the .d file.
    make_path(dirname($dep_file));
    open my $out, '>', $dep_file or die "Cannot write $dep_file: $!";

    # Make target uses a relative obj/ path so generated .d files are portable.
    my $obj_file = catfile('obj', $rel);
    $obj_file =~ s/\.(cpp|cxx|cc|c\+\+)$/.o/;

    print $out "$obj_file: $source_file";

    foreach my $include (keys %includes) {
        # Prefer checked-in headers, then generated headers under build/include.
        my $include_path = catfile($include_dir, $include);
        if (-f $include_path) {
            print $out " $include_path";
            next;
        }

        my $build_inc_path = catfile($build_include_dir, $include);
        if (-f $build_inc_path) {
            print $out " $build_inc_path";
            next;
        }
    }

    print $out "\n";

    # Show what was found so missing project headers are easy to spot.
    if (%includes) {
        print "  Local includes: " . join(', ', keys %includes) . "\n";
    }
    
    if (%system_includes) {
        print "  System includes: " . join(', ', keys %system_includes) . "\n";
    }

    close $out;
}

print "\nDependency calculation complete!\n";
