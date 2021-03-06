#!/usr/bin/perl

# (c) Salsaman 2004 - 2012

# released under the GPL 3 or later
# see file COPYING or www.gnu.org for details

#Communicate with LiVES and do auto VJing


# syntax is autolives.pl host cmd_port status_port
# e.g. autolives.pl localhost 49999 49998
# or just autolives.pl to use defaults 

$DEBUG=1;


$remote_host="localhost";
$remote_port=49999; #command port to app
$local_port=49998; #status port from app

if (defined($ARGV[0])) {
    $remote_host=$ARGV[0];
}

if (defined($ARGV[1])) {
    $remote_port=$ARGV[1];
}

if (defined($ARGV[2])) {
    $local_port=$ARGV[2];
}

if ($^O eq "MSWin32") {
    $sendOMC="perl sendOSC.pl $remote_host $remote_port";
}
else {
    $sendOMC="sendOSC -h $remote_host $remote_port";
}



###################
# ready our listener
use IO::Socket;
use IO::Select;

my $s=new IO::Select;

chop($hostname = `hostname`);  
(undef,undef,undef,undef,$myaddr) = gethostbyname($hostname);
 
@my_ip = unpack("C4", $myaddr);
$my_ip_addr  = join(".", @my_ip);

if ($remote_host eq "localhost") {
    $my_ip_addr="localhost";
}


my $ip1=IO::Socket::INET->new(LocalPort => $local_port, Proto=>'udp',
        LocalAddr => $my_ip_addr)
    or die "error creating UDP listener for $my_ip_addr  $@\n";
$s->add($ip1);


$timeout=1;



#################################################################
# start sending OMC commands

if ($^O eq "MSWin32") {
    `$sendOMC /lives/open_status_socket s $my_ip_addr i $local_port`;
}
else {
    `$sendOMC /lives/open_status_socket,$my_ip_addr,$local_port`;
}


`$sendOMC /lives/ping`;
my $retmsg=&get_newmsg;

print "got $retmsg\n\n";

unless ($retmsg eq "pong") {
    print "Could not connect to LiVES\n";
    exit 2;
}



##loop
#read from keyboard


while (1) {
    $cmd= <STDIN>;
    chomp ($cmd);

    last if ($cmd eq "bye");

    `$sendOMC $cmd`;

    $resp=&get_newmsg;

    print "$resp\n";
}

exit 0;

#####################################################################





sub get_newmsg {
    my $newmsg;
    foreach $server($s->can_read($timeout)){
	$server->recv($newmsg,1024);
	my ($rport,$ripaddr) = sockaddr_in($server->peername);
	
	# TODO - check from address is our host
	#print "FROM : ".inet_ntoa($ripaddr)."($rport)  ";
	
	last;
    }
    # remove terminating NULL
    $newmsg=substr($newmsg,0,length($newmsg)-1);
    chomp ($newmsg);
    return $newmsg;
}





sub location {
    # return the location of an executable
    my ($command)=@_;
    my ($location)=`which $command 2>/dev/null`;
    chomp($location);
    $location;
}



