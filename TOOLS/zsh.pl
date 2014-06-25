#!/usr/bin/perl

# Generate ZSH completion

use strict;
use warnings;

my $mpv = $ARGV[0] || 'mpv';

my @opts = parse_opts("$mpv --list-options", '^ (\-\-[^\s\*]*)\*?\s*(.*)');

my @ao = parse_opts("$mpv --ao=help", '^  ([^\s\:]*)\s*: (.*)');
my @vo = parse_opts("$mpv --vo=help", '^  ([^\s\:]*)\s*: (.*)');

my @af = parse_opts("$mpv --af=help", '^  ([^\s\:]*)\s*: (.*)');
my @vf = parse_opts("$mpv --vf=help", '^  ([^\s\:]*)\s*: (.*)');

my ($opts_str, $ao_str, $vo_str, $af_str, $vf_str);

$opts_str .= qq{  '$_': \\\n} foreach (@opts);
chomp $opts_str;

$ao_str .= qq{      '$_' \\\n} foreach (@ao);
chomp $ao_str;

$vo_str .= qq{      '$_' \\\n} foreach (@vo);
chomp $vo_str;

$af_str .= qq{      '$_' \\\n} foreach (@af);
chomp $af_str;

$vf_str .= qq{      '$_' \\\n} foreach (@vf);
chomp $vf_str;

my $tmpl = <<"EOS";
#compdef mpv

# mpv zsh completion

_x_arguments -C -s \\
$opts_str
  '*:files:->mfiles'

case \$state in
  ao)
    local -a values
    values=(
$ao_str
    )

    _describe -t values 'audio outputs' values
  ;;

  vo)
    local -a values
    values=(
$vo_str
    )

    _describe -t values 'video outputs' values
  ;;

  af)
    local -a values
    values=(
$af_str
    )

    _describe -t values 'audio filters' values
  ;;

  vf)
    local -a values
    values=(
$vf_str
    )

    _describe -t values 'video filters' values
  ;;

  mfiles)
    _tags files urls
    while _tags; do
      _requested files expl 'media file' _files -g \\
         "*.(#i)(asf|asx|avi|flac|flv|m1v|m2p|m2v|m4v|mjpg|mka|mkv|mov|mp3|mp4|mpe|mpeg|mpg|ogg|ogm|ogv|qt|rm|ts|vob|wav|webm|wma|wmv)(-.)" && ret=0
      if _requested urls; then
        while _next_label urls expl URL; do
          _urls "\$expl[@]" && ret=0
          compadd -S '' "\$expl[@]" \{dvd,vcd,cdda,cddb,tv\}:// && ret=0
        done
      fi
      (( ret )) || return 0
    done
  ;;
esac
EOS

print $tmpl;

sub parse_opts {
	my ($cmd, $regex) = @_;

	my @list;
	my @lines = split /\n/, `$cmd`;

	foreach my $line (@lines) {
		if ($line !~ /^$regex/) {
			next;
		}

		my $entry = "$1:";

		if (defined $2) {
			my $desc = $2;
			$desc =~ s/\:/\\:/;

			$entry .= $desc;
		}

		$entry .= ':->ao' if ($1 eq '--ao');
		$entry .= ':->vo' if ($1 eq '--vo');
		$entry .= ':->af' if ($1 eq '--af');
		$entry .= ':->vf' if ($1 eq '--vf');

		push @list, $entry if ($line =~ /^$regex/)
	}

	return @list;
}
