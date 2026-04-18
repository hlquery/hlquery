package make::configure::Extras;

use v5.26.0;
use strict;
use warnings FATAL => qw(all);

use Exporter qw(import);
use File::Basename qw(basename);
use File::Path qw(mkpath);
use File::Spec::Functions qw(abs2rel catfile catdir);

our @EXPORT_OK = qw(
    discover_available_extra_modules
    discover_default_enabled_extras
    list_known_extra_module_names
    parse_enabled_extras
    sync_enabled_extra_module_links
);

sub discover_available_extra_modules {
    my ($base_path) = @_;
    my $extras_root = catdir($base_path, 'src', 'modules', 'extra');
    my %available = (
        simple => {},
        dir    => {},
    );

    return \%available unless -d $extras_root;

    for my $path (glob(catfile($extras_root, 'm_*.cpp'))) {
        next unless -f $path;
        my $name = basename($path);
        $name =~ s/\.cpp$//;
        $available{simple}{$name} = $path;
    }

    for my $path (glob(catdir($extras_root, 'm_*'))) {
        next unless -d $path;
        my $name = basename($path);
        my @sources = grep { -f $_ } glob(catfile($path, '*.cpp'));
        next unless @sources;
        $available{dir}{$name} = [ sort @sources ];
    }

    return \%available;
}

sub discover_default_enabled_extras {
    my ($base_path) = @_;
    my $available = discover_available_extra_modules($base_path);
    my %known = map { $_ => 1 } (keys(%{$available->{simple}}), keys(%{$available->{dir}}));
    my $modules_conf = catfile($base_path, 'run', 'conf', 'modules.conf');
    my @enabled;

    return [] unless -f $modules_conf;

    open my $modules_fh, '<', $modules_conf or return [];
    while (my $line = <$modules_fh>) {
        next if $line =~ /^\s*#/;

        if ($line =~ /<module\s+name="([^"]+)"/i) {
            my $name = lc $1;
            my @candidates = ($name, 'm_' . $name);
            my ($matched) = grep { $known{$_} } @candidates;
            next unless defined $matched;
            push @enabled, $matched;
        }
    }
    close $modules_fh;

    my %seen;
    @enabled = grep { !$seen{$_}++ } @enabled;
    return \@enabled;
}

sub parse_enabled_extras {
    my (%args) = @_;
    my $base_path = $args{base_path};
    my $raw_value = $args{raw_value};
    my $available = discover_available_extra_modules($base_path);

    if (!defined $raw_value || !length $raw_value) {
        my $defaults = discover_default_enabled_extras($base_path);
        $raw_value = join(',', @$defaults) if @$defaults;
    }

    return {
        names       => [],
        simple_srcs => [],
        dir_srcs    => [],
        libs        => [],
        rules       => [],
        sources     => [],
    } unless defined $raw_value && length $raw_value;

    my @requested = grep { length $_ } map { lc $_ } split(/\s*,\s*/, $raw_value);
    if (grep { $_ eq 'all' } @requested) {
        @requested = sort(keys(%{$available->{simple}}), keys(%{$available->{dir}}));
    }

    my %seen;
    @requested = grep { !$seen{$_}++ } @requested;

    my @unknown = grep { !exists $available->{simple}{$_} && !exists $available->{dir}{$_} } @requested;
    if (@unknown) {
        my @known = sort(keys(%{$available->{simple}}), keys(%{$available->{dir}}));
        die "Unknown extra module(s): " . join(', ', @unknown) . "\n"
          . "Available extras: " . (@known ? join(', ', @known) : '(none)') . "\n";
    }

    my (@simple_srcs, @dir_srcs, @libs, @rules, @sources);

    for my $name (@requested) {
        if (exists $available->{simple}{$name}) {
            push @simple_srcs, '$(SRC_DIR)/modules/extra/' . $name . '.cpp';
            push @libs, '$(RUN_DIR)/modules/' . $name . '.so';
            push @rules,
                '$(RUN_DIR)/modules/' . $name . '.so: $(OBJ_DIR)/modules/extra/' . $name . '.module.o $(ROCKSDB_LIB) | $(BIN_DIR)',
                "\t" . '@mkdir -p $(RUN_DIR)/modules',
                "\t" . '@echo "$(CYAN)Linking extra module ' . $name . '...$(NC)"',
                "\t" . '$(CXX) -shared -o $@ $^ $(CONFIGURE_LDFLAGS) $(MODULE_SHARED_LDFLAGS) $(MODULE_EXTRA_LDFLAGS)',
                '';
            push @sources, $available->{simple}{$name};
            next;
        }

        next unless exists $available->{dir}{$name};
        my @module_sources = @{$available->{dir}{$name}};
        push @dir_srcs, map {
            my $rel = abs2rel($_, catdir($base_path, 'src', 'modules'));
            '$(SRC_DIR)/modules/' . $rel;
        } @module_sources;
        my @module_objs = map {
            my $rel = abs2rel($_, catdir($base_path, 'src', 'modules'));
            $rel =~ s/\.cpp$/.module.o/;
            '$(OBJ_DIR)/modules/' . $rel;
        } @module_sources;
        push @libs, '$(RUN_DIR)/modules/' . $name . '.so';
        push @rules,
            '$(RUN_DIR)/modules/' . $name . '.so: ' . join(' ', @module_objs) . ' $(ROCKSDB_LIB) | $(BIN_DIR)',
            "\t" . '@mkdir -p $(RUN_DIR)/modules',
            "\t" . '@echo "$(CYAN)Linking extra module ' . $name . '...$(NC)"',
            "\t" . '$(CXX) -shared -o $@ $^ $(CONFIGURE_LDFLAGS) $(MODULE_SHARED_LDFLAGS) $(MODULE_EXTRA_LDFLAGS)',
            '';
        push @sources, @module_sources;
    }

    return {
        names       => \@requested,
        simple_srcs => \@simple_srcs,
        dir_srcs    => \@dir_srcs,
        libs        => \@libs,
        rules       => \@rules,
        sources     => \@sources,
    };
}

sub list_known_extra_module_names {
    my ($base_path) = @_;
    my $available = discover_available_extra_modules($base_path);
    return [ sort(keys(%{$available->{simple}}), keys(%{$available->{dir}})) ];
}

sub sync_enabled_extra_module_links {
    my (%args) = @_;
    my $base_path = $args{base_path};
    my $enabled_extras = $args{enabled_extras} // {};
    my $available = discover_available_extra_modules($base_path);
    my $modules_root = catdir($base_path, 'src', 'modules');

    mkpath($modules_root) unless -d $modules_root;

    for my $name (@{list_known_extra_module_names($base_path)}) {
        my $target_path = catfile($modules_root, $name);
        $target_path .= '.cpp' if exists $available->{simple}{$name};

        my $enabled = grep { $_ eq $name } @{$enabled_extras->{names} // []};
        if (!$enabled) {
            unlink $target_path if -l $target_path;
            next;
        }

        my $link_target = exists $available->{simple}{$name}
            ? catfile('extra', $name . '.cpp')
            : catfile('extra', $name);

        if (-l $target_path) {
            my $current = readlink($target_path);
            next if defined $current && $current eq $link_target;
            unlink $target_path;
        } elsif (-e $target_path) {
            warn "Warning: refusing to replace non-symlink path $target_path while enabling extra module $name\n";
            next;
        }

        symlink($link_target, $target_path)
            or die "Failed to create symlink $target_path -> $link_target: $!\n";
    }
}

1;
