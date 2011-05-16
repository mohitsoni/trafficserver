#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.


############################################################################
# This is a simple module to let you read, modify and add to an Apache
# Traffic Server records.config file. The idea is that you would write a
# simple script (like example below) to update a "stock" records.config with
# the changes applicable to your application. This allows you to uprade to
# a newer default config file from a new release, for example.
#
#
# #!/usr/bin/perl
#
# use Apache::TS::Config::Records;
#
# my $recedit = new Apache::TS::Config::Records(file => "/tmp/records.config");
# $recedit->set(conf => "proxy.config.log.extended_log_enabled",
#               val => "123");
# $recedit->write(file => "/tmp/records.config.new");
#
############################################################################

package Apache::TS::Config::Records;

use warnings;
use strict;

require 5.006;

use Carp;

our $VERSION = "1.0";


# This should get moved to a TS::Config.pm module (one level above here).
use constant {
    TS_CONF_UNMODIFIED     => 0,
    TS_CONF_MODIFIED       => 1,
    TS_CONF_REMOVED        => 2
};


#
# Constructor
#
sub new {
    my ($class, %args) = @_;
    my $self = {};
    my $f = $args{file} || $args{filename} || $_[0] || "-";

    $self->{_filename} = $f;  # Filename to open when loading and saving
    $self->{_configs} = [];            # Storage, and to to preserve order
    $self->{_lookup} = {};             # For faster lookup, indexes into the above
    $self->{_ix} = -1;                 # Empty
    bless $self, $class;

    $self->load() if $self->{_filename};

    return $self;
}


#
# Load a records.config file
#
sub load {
    my $self = shift;
    my %args = @_;
    my $filename = $args{filename} || $self->{_filename} || die "Need a filename to load";

    open(FH, "<$filename");
    while (<FH>) {
        chomp;
        my @p = split(/\s+/, $_, 4);

        push(@{$self->{_configs}}, [$_, \@p, TS_CONF_UNMODIFIED]);

        ++($self->{_ix});
        next unless ($#p == 3) && (($p[0] eq "LOCAL") || ($p[0] eq "CONFIG"));
        print "Warning! duplicate configuration $p[1]\n" if exists($self->{_lookup}->{$p[1]});
      
        $self->{_lookup}->{$p[1]} = $self->{_ix};
    }
}


#
# Get an existing configuration line. This is useful for
# detecting that a config exists or not, for example. The
# return value is an anonymous array like
#
#    [<line string>, [value split into 4 fields, flag if changed]
#
# You probably shouldn't modify this array.
#
sub get {
    my $self = shift;
    my %args = @_;
    my $c = $args{conf} || $args{config} || $_[0];
    my $ix = $self->{_lookup}->{$c};

    return [] unless defined($ix);
    return $self->{_configs}->[$ix];
}


#
# Modify one configuration value
#
sub set {
    my $self = shift;
    my %args = @_;
    my $c = $args{conf} || $args{config} || $_[0];
    my $v = $args{val} || $args{value} || $_[1];
    my $ix = $self->{_lookup}->{$c};

    if (!defined($ix)) {
        print "Error: set(): No such configuration exists: $c\n";
        return;
    }

    my $val = $self->{_configs}->[$ix];

    @{$val->[1]}[3] = $v;
    $val->[2] = TS_CONF_MODIFIED;
}


#
# Remove a configuration from the file.
#
sub remove {
    my $self = shift;
    my %args = @_;
    my $c = $args{conf} || $args{config} || $_[0];

    my $ix = $self->{_lookup}->{$c};

    $self->{_configs}->[$ix]->[2] = TS_CONF_REMOVED if defined($ix);
}


#
# Append anything to the "end" of the configuration. We will assure that
# no duplicated configurations are added. Note that this takes a single
# line.
#
sub append {
    my $self = shift;
    my %args = @_;
    my $line = $args{line} || $_[0];

    my @p = split(/\s+/, $line, 4);

    # Don't appending duplicated configs
    if (($#p == 3) && exists($self->{_lookup}->{$p[1]})) {
        print "Warning: duplicate configuration $p[1]\n";
        return;
    }

    push(@{$self->{_configs}}, [$line, \@p, TS_CONF_UNMODIFIED]);
    ++($self->{_ix});
    $self->{_lookup}->{$p[1]} = $self->{_ix} if ($#p == 3) && (($p[0] eq "LOCAL") || ($p[0] eq "CONFIG"));
}


#
# Write the new configuration file to STDOUT, or provided 
#
sub write {
    my $self = shift;
    my %args = @_;
    my $filename = $args{file} || $args{filename} || $_[0] || "-";

    if ($filename ne "-") {
        close(STDOUT);
        open(STDOUT, ">$filename") || die "Can't open $filename for writing";
    }

    foreach (@{$self->{_configs}}) {
        if ($_->[2] == TS_CONF_UNMODIFIED) {
            print $_->[0], "\n";
        } elsif ($_->[2] == TS_CONF_MODIFIED) {
            print join(" ", @{$_->[1]}), "\n";
        } else {
            # No-op if removed
        }
    }
}
