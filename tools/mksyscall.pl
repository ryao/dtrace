#! /usr/bin/perl

# $Header:$

# Handle syscall names which vary in location and available from
# one kernel and architecture to the next.

# Author: Paul Fox
# Date: June 2008

# 26-Jan-2011 PDF Need to generate both i386 and amd64 syscall tables
#                 if this is a 64b kernel, because 64b kernel can run
#                 32b apps.
# 21-Jun-2011 PDF Changes to better handle asm-i386 and 2.6.18 kernels
# 08-Apr-2012 PDF Add fix for 3.3 syscalls. (We will need to support ia-32 sometime)

use strict;
use warnings;

use File::Basename;
use FileHandle;
use Getopt::Long;
use IO::File;
use POSIX;

#######################################################################
#   Command line switches.					      #
#######################################################################
my %opts;

sub main
{
	Getopt::Long::Configure('no_ignore_case');
	usage() unless GetOptions(\%opts,
		'help',
		'n',
		'tmp',
		'ver=s',
		);

	usage() if ($opts{help});

	if (!$ENV{BUILD_DIR} && !$opts{n} && !$opts{tmp}) {
		die "\$BUILD_DIR must be defined before running this script";
	}

	my $ver = $opts{ver};
	$ver = `uname -r` if !$ver;
	chomp($ver);

	foreach my $bits (qw/32 64/) {
		my $name = $bits == 32 ? "x86" : "x86-64";
#	        my $machine = `uname -m`;
#	        if ($machine =~ /x86_64/) {
#	        	$bits = 64;
#	        } elsif ($machine =~ /i[34567]86/) {
#	        	$bits = 32;
#	        } else {
#	        	die "Unexpected machine: $machine";
#	        }

		my %calls;
		my @unistd_h_candidates = get_unistd($bits, $ver);

	        my $syscall_count = 0;
		my @src_list;
	        foreach my $f (@unistd_h_candidates) {
			if (! -e $f) {
				print "(no file: $f)\n";
				next;
			}

			print "Processing ($bits): $f\n";
			my $fh = new FileHandle($f);
			if (!$fh) {
				die "Cannot open $f: $!";
			}
			while (<$fh>) {
				next if !/define\s+(__NR[A-Z_a-z0-9]+)\s+(.*)/;
				my ($name, $val) = ($1, $2);
				next if defined($calls{$name});
				my $val2 = map_define($val, $name, \%calls);
				next if !defined($val2);
				$calls{$name} = $val2;
	                        $syscall_count += 1;
			}
			###############################################
			#   We  may  hit  unistd.h  which  in  turn,  #
			#   includes  unistd_32.h or unistd_64.h, so  #
			#   see  if  we  can go for one of the other  #
			#   files, if we got nothing useful.	      #
			###############################################
			push @src_list, $f;
	                last if scalar(keys(%calls));
		}


		###############################################
		#   Create an empty file, even if we are 32b  #
		#   kernel, and have no 64b syscalls.	      #
		###############################################
		my $dir = dirname($0);
		next if $opts{n};

		my $fname = $opts{tmp}
				? "/tmp/syscalls-$name.tbl"
				: "$ENV{BUILD_DIR}/driver/syscalls-$name.tbl";
		my $fh = new FileHandle(">$fname");
		die "Cannot create: $fname -- $!" if !$fh;

	        # Make sure we've found reasonable number of system calls.
	        # 2.6.15 i386 has 300+, x86_64 has 255
		if ($syscall_count < 200) {
		        warn "mksyscall.pl: [$name] Unable to generate syscall table, syscall_count==$syscall_count, which looks\nsuspiciously too low. Might have misparsed the sys_call_table\n";
			next;
		}

		print "Creating: $fname - ", scalar(keys(%calls)), " entries\n";

		print $fh "/* This file is automatically generated from mksyscall.pl */\n";
		print $fh "/* Source: $_ */\n" foreach @src_list;
		print $fh "/* Do not edit! */\n";
		my %vals;
		foreach my $c (keys(%calls)) {
			$vals{$calls{$c}} = $c;
		}
		my $n = 0;
		foreach my $c (sort {$a <=> $b} (keys(%vals))) {
			my $name = $vals{$c};
			$name =~ s/^__NR_//;
			$name =~ s/^ia32_//;
			my $val = $vals{$c};
			while ($n < $c) {
				print $fh "/* gap: no syscall $n */\n";
				$n++;
			}
			print $fh " [$c] = \"$name\",\n";
			$n = $c + 1;
		}
		###############################################
		#   We  need  __NR_xxx  for  the  64-bit and  #
		#   32-bit  scenarios, but we cannot include  #
		#   both   unistd_32.h  and  unistd_64.h  so  #
		#   handle this here.			      #
		###############################################
		if ($bits == 32) {
			foreach my $c (sort {$a <=> $b} (keys(%vals))) {
				my $name = $vals{$c};
				$name =~ s/^__NR_//;
				$name =~ s/^ia32_//;
				my $val = $vals{$c};
				print $fh "#define NR_ia32_$name $c\n";
			}
		}
	}

}
######################################################################
#   Try  and  locate the relevant unistd.h for this release/system.  #
#   The kernel moved these around and renamed them - and its not as  #
#   simple as you might want.					     #
######################################################################
sub get_unistd
{	my $bits = shift;
	my $ver = shift;

	###############################################
	#   Linux 3.3 handling.			      #
	###############################################
	if ($bits == 32 &&
	    -f "/lib/modules/$ver/build/arch/x86/include/generated/asm/unistd_32.h") {
	    return ("/lib/modules/$ver/build/arch/x86/include/generated/asm/unistd_32.h");
	}
	if ($bits == 64 &&
	    -f "/lib/modules/$ver/build/arch/x86/include/generated/asm/unistd_64.h") {
	    return ("/lib/modules/$ver/build/arch/x86/include/generated/asm/unistd_64.h");
	}

	###############################################
	#   OpenSuse bizarreness.		      #
	###############################################
	my $ver2 = $ver;
	$ver2 =~ s/-[a-z]*$//;
        my @unistd_h_candidates;
	foreach my $f (
	     # 2.6.9-78.EL
             "/lib/modules/$ver/build/include/asm-x86_64/ia${bits}_unistd.h",
             # linux-2.6.15, 2.6.23:
#	     "/lib/modules/$ver/build/include/asm/unistd.h",
             # linux-2.6.26:
             "/lib/modules/$ver/build/include/asm-x86/unistd_$bits.h",
             # linux-2.6.28-rc7:
             "/lib/modules/$ver/build/arch/x86/include/asm/unistd_$bits.h",
	     # Opensuse 11.1 wants this
	     "/usr/src/linux-$ver2/arch/x86/include/asm/unistd_$bits.h",
             ) {
	     	next if ! -f $f;
		next if $bits == 32 && $f =~ /ia32_/;
		push @unistd_h_candidates, $f;
	}
	if ($bits == 32) {
		foreach my $f (
	             "/lib/modules/$ver/build/include/asm-i386/unistd.h",
	             "/lib/modules/$ver/build/include/asm-x86_64/ia32_unistd.h",
		     ) {
			next if ! -f $f;
			push @unistd_h_candidates, $f;
		}
	}
	if ($bits == 64) {
		foreach my $f (
	             "/lib/modules/$ver/build/include/asm-x86_64/unistd.h",
		     ) {
			next if ! -f $f;
			push @unistd_h_candidates, $f;
		}
	}
	return @unistd_h_candidates;
}
######################################################################
#   Map  a  #define,  attempting  to  realise macro substitutions -  #
#   enough  to  get  the  job  done. Some of the unistd.h files are  #
#   overly complex.						     #
######################################################################
sub map_define
{	my $val = shift;
	my $name = shift;
	my $calls = shift;

	$val =~ s/\s+.*$//;
	return $val if $val =~ /^\d+$/;
	$val =~ s/[()]//g;
	if ($val =~ /^(.*)\+(\d+)/) {
		my ($name, $addend) = ($1, $2);
		return $calls->{$name} + $addend;
	}
	return if $name eq '__NR_syscall_max';
	return if $name eq '__NR__exit';
	print "Line $.: warning - unknown value: $name=$val\n";
	return;
}
#######################################################################
#   Print out command line usage.				      #
#######################################################################
sub usage
{
	print <<EOF;
mksyscall.pl: Compile up the sys_call_table string entries for the driver.
Usage: mksyscall.pl

Switches:

   -n             Dont generate files - just tell us where we are going.
   -tmp           Write output files to /tmp
   -ver X.Y.Z     Override the uname -r of the current kernel.

Examples:

   \$ tools/mksyscall.pl -ver 2.6.18-164.el5-i686 -tmp
EOF
	exit(1);
}

main();
0;

