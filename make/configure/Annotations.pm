package make::configure::Annotations;

use v5.26.0;
use strict;
use warnings FATAL => qw(all);

use Exporter qw(import);
use File::Spec::Functions qw(abs2rel catfile catdir);
use make::configure::System qw(system_pattern_matches);

our @EXPORT_OK = qw(collect_module_annotations);

sub parse_annotation_arguments {
    my ($raw) = @_;
    my @args = ($raw =~ /"((?:[^"\\]|\\.)*)"/g);
    @args = map {
        my $v = $_;
        $v =~ s/\\"/"/g;
        $v =~ s/\\\\/\\/g;
        $v;
    } @args;
    return @args;
}

sub collapse_annotation_value {
    my ($value) = @_;
    $value //= '';
    $value =~ s/\s+/ /g;
    $value =~ s/^\s+//;
    $value =~ s/\s+$//;
    return $value;
}

sub quote_makefile_value {
    my ($value) = @_;
    $value //= '';
    $value =~ s/\$/\$\$/g;
    return $value;
}

sub resolve_annotation_value {
    my (%args) = @_;
    my $expr = collapse_annotation_value($args{expr});
    my $source = $args{source};
    my $kind = $args{kind};
    my $debug = $args{debug};
    my $on_success = $args{on_success};
    my $on_warning = $args{on_warning};

    return '' unless length $expr;

    if ($expr =~ /^find_compiler_flags\s*\((.*)\)\s*$/) {
        my @args = parse_annotation_arguments($1);
        return '' unless @args;

        my $package = $args[0];
        my $label = $args[1] // $package;
        my $fallback = $args[2] // '';
        my $candidates = [
            ['pkg-config', '--cflags', $package],
            ['pkgconf', '--cflags', $package],
        ];

        for my $command (@$candidates) {
            my $output = `@$command 2>/dev/null`;
            my $exit_code = $? >> 8;
            $output = collapse_annotation_value($output);

            if ($exit_code == 0 && length $output) {
                $on_success->("Module $kind flags", "$source => $output")
                    if $debug && $on_success;
                return $output;
            }
        }

        if (length $fallback) {
            $on_warning->("Module $kind flags", "$source => fallback $fallback")
                if $debug && $on_warning;
            return collapse_annotation_value($fallback);
        }

        $on_warning->("Module $kind flags", "$source => unavailable ($label)")
            if $debug && $on_warning;
        return '';
    }

    if ($expr =~ /^find_linker_flags\s*\((.*)\)\s*$/) {
        my @args = parse_annotation_arguments($1);
        return '' unless @args;

        my $package = $args[0];
        my $label = $args[1] // $package;
        my $fallback = $args[2] // '';
        my $candidates = [
            ['pkg-config', '--libs', $package],
            ['pkgconf', '--libs', $package],
        ];

        for my $command (@$candidates) {
            my $output = `@$command 2>/dev/null`;
            my $exit_code = $? >> 8;
            $output = collapse_annotation_value($output);

            if ($exit_code == 0 && length $output) {
                $on_success->("Module $kind flags", "$source => $output")
                    if $debug && $on_success;
                return $output;
            }
        }

        if (length $fallback) {
            $on_warning->("Module $kind flags", "$source => fallback $fallback")
                if $debug && $on_warning;
            return collapse_annotation_value($fallback);
        }

        $on_warning->("Module $kind flags", "$source => unavailable ($label)")
            if $debug && $on_warning;
        return '';
    }

    if ($expr =~ /^execute\s*\((.*)\)\s*$/) {
        my @args = parse_annotation_arguments($1);
        return '' unless @args;

        my $command = shift @args;
        my $label = @args ? shift @args : '';
        my $fallback = @args ? shift @args : '';
        my $output = `$command 2>/dev/null`;
        my $exit_code = $? >> 8;
        $output = collapse_annotation_value($output);

        if ($exit_code == 0 && length $output) {
            $on_success->("Module $kind flags", "$source => $output")
                if $debug && $on_success;
            return $output;
        }

        if (length $fallback) {
            $on_warning->("Module $kind flags", "$source => fallback $fallback")
                if $debug && $on_warning;
            return collapse_annotation_value($fallback);
        }

        my $desc = $label || $command;
        $on_warning->("Module $kind flags", "$source => unavailable ($desc)")
            if $on_warning;
        return '';
    }

    return $expr;
}

sub collect_module_annotations {
    my (%args) = @_;
    my $base_path = $args{base_path};
    my $layout = $args{layout};
    my $enabled_extras = $args{enabled_extras} // {};
    my $debug = $args{debug};
    my $on_success = $args{on_success};
    my $on_warning = $args{on_warning};

    my @module_sources;
    my $modules_root = catdir($base_path, 'src', 'modules');
    push @module_sources, grep { -f $_ } glob(catfile($modules_root, 'm_*.cpp'));
    push @module_sources, grep { -f $_ } glob(catfile($modules_root, 'm_*', '*.cpp'));
    push @module_sources, @{$enabled_extras->{sources} // []};

    my (%source_cxxflags, %module_ldflags, %module_packages);

    for my $source (@module_sources) {
        my $relative = abs2rel($source, $modules_root);
        my $stem = $relative;
        $stem =~ s/\.cpp$//;
        my $module_name = $relative;
        $module_name =~ s{/.*$}{};
        $module_name =~ s/\.cpp$//;

        open my $fh, '<', $source or next;
        while (my $line = <$fh>) {
            next unless $line =~ m{^\s*///\s*\$(CompilerFlags|LinkerFlags|PackageInfo):\s*(.*?)\s*$};
            my ($kind, $payload) = ($1, $2);

            if ($kind eq 'CompilerFlags') {
                my $value = resolve_annotation_value(
                    expr       => $payload,
                    source     => $relative,
                    kind       => 'compiler',
                    debug      => $debug,
                    on_success => $on_success,
                    on_warning => $on_warning,
                );
                push @{$source_cxxflags{$stem}}, $value if length $value;
            } elsif ($kind eq 'LinkerFlags') {
                my $value = resolve_annotation_value(
                    expr       => $payload,
                    source     => $relative,
                    kind       => 'linker',
                    debug      => $debug,
                    on_success => $on_success,
                    on_warning => $on_warning,
                );
                push @{$module_ldflags{$module_name}}, $value if length $value;
            } elsif ($kind eq 'PackageInfo') {
                if ($payload =~ /^require_system\("([^"]+)"\)\s+(.+?)\s*$/) {
                    my ($pattern, $package) = (lc($1), collapse_annotation_value($2));
                    if (system_pattern_matches($pattern, $layout)) {
                        push @{$module_packages{$module_name}}, $package if length $package;
                    }
                }
            }
        }
        close $fh;
    }

    my @cxx_rules;
    for my $stem (sort keys %source_cxxflags) {
        my %seen;
        my @flags = grep { length $_ && !$seen{$_}++ } @{$source_cxxflags{$stem}};
        next unless @flags;
        push @cxx_rules, '$(OBJ_DIR)/modules/' . $stem . '.module.o: CXXFLAGS += '
            . quote_makefile_value(join(' ', @flags));
    }

    my @ld_rules;
    for my $module (sort keys %module_ldflags) {
        my %seen;
        my @flags = grep { length $_ && !$seen{$_}++ } @{$module_ldflags{$module}};
        next unless @flags;
        push @ld_rules, '$(RUN_DIR)/modules/' . $module . '.so: MODULE_EXTRA_LDFLAGS += '
            . quote_makefile_value(join(' ', @flags));
    }

    my @package_lines;
    for my $module (sort keys %module_packages) {
        my %seen;
        my @packages = grep { length $_ && !$seen{$_}++ } @{$module_packages{$module}};
        next unless @packages;
        push @package_lines, $module . ': ' . join(', ', @packages);
    }

    return {
        cxx_rules     => join("\n", @cxx_rules),
        ld_rules      => join("\n", @ld_rules),
        package_lines => \@package_lines,
    };
}

1;
