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

package make::configure::system;

use v5.26.0;
use strict;
use warnings FATAL => qw(all);

use Exporter qw(import);
use File::Spec::Functions qw(catdir catfile);

our @EXPORT_OK = qw(
    apply_install_layout
    default_cxx
    detect_packaging_layout
    get_cpu_count
    get_make_command
    get_os_release_info
    get_system_name
    get_version_from_script
    system_pattern_matches
);

my $detected_system = `uname -s 2>/dev/null`;
chomp $detected_system;
$detected_system ||= $^O;

sub get_system_name {
    return $detected_system;
}

sub get_make_command {
    return get_system_name() eq 'FreeBSD' ? 'gmake' : 'make';
}

sub get_cpu_count {
    my $count = 0;

    if (get_system_name() eq 'FreeBSD') {
        $count = `sysctl -n hw.ncpu 2>/dev/null`;
    } else {
        $count = `nproc 2>/dev/null`;
    }

    chomp $count if defined $count;
    return ($count && $count =~ /^\d+$/ && $count > 0) ? $count : 1;
}

sub default_cxx {
    return $ENV{CXX} if $ENV{CXX};
    return 'c++';
}

sub get_version_from_script {
    my ($base_path) = @_;
    my $version_script = defined $base_path
        ? catfile($base_path, 'src', 'version.sh')
        : catfile('src', 'version.sh');

    if (-f $version_script) {
        my $version_output = `bash "$version_script" 2>/dev/null`;
        chomp $version_output;
        if ($version_output && $version_output !~ /error/i) {
            $version_output =~ s/^hlquery-//;
            return $version_output;
        }
    }

    if (-f $version_script) {
        open my $fh, '<', $version_script or return '1.0.0';
        while (my $line = <$fh>) {
            if ($line =~ /VERSION_STRING=['"](.+?)['"]/) {
                my $version = $1;
                $version =~ s/^hlquery-//;
                close $fh;
                return $version;
            }
        }
        close $fh;
    }

    return '1.0.0';
}

sub detect_packaging_layout {
    return 'system' unless get_system_name() eq 'Linux';

    my $os_release = '/etc/os-release';
    return 'system' unless -f $os_release;

    my ($id, $id_like) = ('', '');
    if (open my $fh, '<', $os_release) {
        while (my $line = <$fh>) {
            chomp $line;
            if ($line =~ /^ID=(.*)$/) {
                $id = $1;
                $id =~ s/^"(.*)"$/$1/;
            } elsif ($line =~ /^ID_LIKE=(.*)$/) {
                $id_like = $1;
                $id_like =~ s/^"(.*)"$/$1/;
            }
        }
        close $fh;
    }

    my $all = lc("$id $id_like");
    return 'debian' if $all =~ /\b(debian|ubuntu|mint|pop|kali)\b/;
    return 'rpm' if $all =~ /\b(rhel|fedora|centos|rocky|almalinux|suse|opensuse)\b/;

    return 'system';
}

sub get_os_release_info {
    my %info;
    my $os_release = '/etc/os-release';
    return %info unless -f $os_release;

    if (open my $fh, '<', $os_release) {
        while (my $line = <$fh>) {
            chomp $line;
            next unless $line =~ /^([A-Z_]+)=(.*)$/;
            my ($key, $value) = (lc($1), $2);
            $value =~ s/^"(.*)"$/$1/;
            $info{$key} = lc($value);
        }
        close $fh;
    }

    return %info;
}

sub system_pattern_matches {
    my ($pattern, $layout) = @_;
    return 0 unless defined $pattern && length $pattern;

    my %os = get_os_release_info();
    my %tokens;

    my $system = lc(get_system_name());
    $tokens{$system} = 1 if $system;
    $tokens{lc($layout)} = 1 if defined $layout && length $layout;

    for my $key (qw(id id_like)) {
        next unless $os{$key};
        for my $token (split /\s+/, $os{$key}) {
            $tokens{$token} = 1 if $token;
        }
    }

    my $normalized = lc($pattern);
    my $prefix = ($normalized =~ s/~$//);

    if ($prefix) {
        for my $token (keys %tokens) {
            return 1 if index($token, $normalized) == 0;
        }
        return 0;
    }

    return $tokens{$normalized} ? 1 : 0;
}

sub apply_install_layout {
    my (%args) = @_;
    my $layout = $args{layout};
    my $config_ref = $args{config};
    my $set_ref = $args{set_ref};
    my $base_path = $args{base_path};
    my $run_dir = $args{run_dir} // 'run';

    die "apply_install_layout requires config and set_ref\n"
        unless ref($config_ref) eq 'HASH' && ref($set_ref) eq 'HASH';

    if ($layout eq 'auto') {
        $layout = detect_packaging_layout();
    }

    if ($layout eq 'dev') {
        $config_ref->{PREFIX} = $base_path unless $set_ref->{prefix};
        $config_ref->{CONFDIR} = catdir($base_path, $run_dir, 'conf') unless $set_ref->{confdir};
        $config_ref->{LOGDIR}  = catdir($base_path, $run_dir, 'logs') unless $set_ref->{logdir};
        $config_ref->{DATADIR} = catdir($base_path, $run_dir, 'data') unless $set_ref->{datadir};
        $config_ref->{RUNDIR}  = catdir($base_path, $run_dir, 'pid') unless $set_ref->{rundir};
        $config_ref->{BINDIR}  = catdir($base_path, $run_dir, 'bin') unless $set_ref->{bindir};
        $config_ref->{SYSTEMD_UNIT_DIR} = '' unless exists $config_ref->{SYSTEMD_UNIT_DIR};
    } elsif ($layout eq 'system' || $layout eq 'debian' || $layout eq 'rpm') {
        $config_ref->{PREFIX} = '/usr' unless $set_ref->{prefix};
        $config_ref->{CONFDIR} = '/etc/hlquery' unless $set_ref->{confdir};
        $config_ref->{LOGDIR}  = '/var/log/hlquery' unless $set_ref->{logdir};
        $config_ref->{DATADIR} = '/var/lib/hlquery' unless $set_ref->{datadir};
        $config_ref->{RUNDIR}  = '/run/hlquery' unless $set_ref->{rundir};
        $config_ref->{BINDIR}  = '/usr/bin' unless $set_ref->{bindir};
        $config_ref->{SYSTEMD_UNIT_DIR} = '/usr/lib/systemd/system';
        $config_ref->{SYSTEMD_UNIT_DIR} = '/lib/systemd/system' if $layout eq 'debian';
    } else {
        die "Invalid layout '$layout'. Valid layouts: dev, system, debian, rpm, auto\n";
    }

    $config_ref->{LAYOUT} = $layout;
    return $layout;
}

1;
