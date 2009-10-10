#!perl
BEGIN { $ENV{DBI_TRACE} = 5; }
use strict;
use warnings;
use DBI;
use blib;
use DBD::Pg;
use AnyEvent;
use Coro;
use Coro::AnyEvent;
use Test::More tests => 41;
use Test::Exception;
use lib 't','.';
require 'dbdpg_test_setup.pl';
select(($|=1,select(STDERR),$|=1)[1]);

my ( $testdsn, $testuser, $helpconnect, $su, $uid, $testdir, $pg_ctl, $initdb, $error) = get_test_settings();
my ($helpconnect,$connerror,$dbh) = connect_database();
my (undef,undef,$dbh2) = connect_database({nosetup => 1});
ok $dbh, "got one dbh";
ok $dbh2, "got two dbh's";
ok $dbh!=$dbh2, "... they're different";
ok $dbh->{pg_socket} ne $dbh2->{pg_socket}, "... they're different sockets";

$dbh->{AutoCommit} = 1;
$dbh2->{AutoCommit} = 1;

END { local $?; $dbh->disconnect; $dbh2->disconnect }

my $table = "test_$^T";
$dbh->begin_work;
my $rv = $dbh->do("CREATE TABLE $table(foo int, bar text not null)");
ok $rv, "got rv";
$dbh->commit;

prepare_and_nullable: {
    my $sth = $dbh->prepare("SELECT * FROM test_$^T");
    ok $sth;
    $sth->execute();
    ok $sth->rows == 0;

    my $nullable = $sth->{NULLABLE};
    ok $nullable, "got nullable ref";
    is $nullable->[0], 1;
    is $nullable->[1], 0;
}

just_one_statement: {
    lives_ok {
        my $rv = $dbh->do(qq{
            INSERT INTO $table (foo,bar) VALUES (-1,'neg one');
            INSERT INTO $table (foo,bar) VALUES (-2,'neg two');
        });
    } 'no exception due to an extra result';
    ok $rv;

    my $sth = $dbh->prepare("SELECT * FROM test_$^T");
    $sth->execute();
    is $sth->rows, 2, "two rows inserted";
}

my $ping = $dbh->pg_ping();
ok $ping > 0, 'pinged';

ok $dbh->do("TRUNCATE $table"), 'truncated';

my $cv = AE::cv;
my $swaps = 0;
for (1..5) {
    $cv->begin;
    async {
        my $n = shift;
        Coro::on_enter {
#             diag "enter $n";
            $swaps++;
        };

        my (undef,undef,$this_dbh) = connect_database({nosetup => 1});
        ok $this_dbh, "$n connected";
#         diag "$n conn";
        $this_dbh->{AutoCommit} = 1;
        ok $this_dbh->do("INSERT INTO $table (foo,bar) VALUES (?,?)", {}, $n,"baz"), "$n inserted";
#         diag "$n do";

        my $sth = $this_dbh->prepare("SELECT * FROM $table WHERE foo = ?");
        ok $sth, "$n prepared a placeholder'd query";

        $sth->execute($n);
#         diag "$n exec";
        ok $sth->rows >= 1, "$n got one or more rows";
        $sth->finish;

        eval { $this_dbh->disconnect };
        ok !$@, "$n disconnected";
        $cv->end;
    } $_;
}
$cv->recv;
cmp_ok $swaps, '>', 5, "did $swaps 'on_enter's";
