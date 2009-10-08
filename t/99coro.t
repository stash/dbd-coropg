#!perl
BEGIN { $ENV{DBI_TRACE} = 1; }
use strict;
use warnings;
use DBI;
use blib;
use DBD::Pg;
use Test::More tests => 6;
use lib 't','.';
require 'dbdpg_test_setup.pl';
select(($|=1,select(STDERR),$|=1)[1]);

my ($helpconnect,$connerror,$dbh) = connect_database();
pass "connected";

END { local $?; $dbh->disconnect }

my $rv = $dbh->do("CREATE TABLE test_$^T(foo int, bar text not null)");
ok $rv, "got rv";

my $sth = $dbh->prepare("SELECT * FROM test_$^T");
ok $sth;
$sth->execute();
ok $sth->rows == 0;

my $nullable = $sth->{NULLABLE};
ok $nullable;

my $ping = $dbh->pg_ping();
ok $ping > 0, 'pinged';
