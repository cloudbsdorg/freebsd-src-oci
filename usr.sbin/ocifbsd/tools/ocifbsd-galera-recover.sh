#!/bin/sh
#
# ocifbsd-galera-recover — automatic MariaDB Galera recovery after a
# full-cluster reboot.
#
# When every node reboots at once, each mariadbd comes up with a saved seqno of
# -1 (unclean shutdown) and none will bootstrap itself — the cluster stays down
# until a human bootstraps one node. This script, run once at boot on every
# node, closes that gap: the designated bootstrap node brings the Primary
# component up if none exists, and the other nodes (re)join it. It is
# idempotent — if a Primary already exists (normal single-node reboot) it does
# nothing and lets the node join normally.
#
# Because Galera replicates synchronously, at the moment of a clean cluster all
# members hold identical data, so a fixed designated bootstrap node is safe.
#
set -u
SELF=${OCIFBSD_GALERA_SELF:?set OCIFBSD_GALERA_SELF to this node's galera ip}
BOOT=${OCIFBSD_GALERA_BOOTSTRAP:-10.88.1.11}
ADDR=${OCIFBSD_GALERA_ADDR:-gcomm://10.88.1.11,10.88.2.11,10.88.3.11}

mj()      { jls jid path 2>/dev/null | awk '/local.mariadb/{print $1}' | head -1; }
rootfs()  { ls -d /var/lib/ocifbsd/docker.io/local/mariadb/latest/rootfs; }
status()  { j=$(mj); [ -n "$j" ] && jexec "$j" mysql --socket=/tmp/mysql.sock -u root -Nse \
              'SHOW STATUS LIKE "wsrep_cluster_status";' 2>/dev/null | awk '{print $2}'; }
mdb_up()  { j=$(mj); jexec "$j" pgrep mariadbd >/dev/null 2>&1; }
start_mdb(){ j=$(mj); jexec "$j" daemon -f -o /var/log/mdb.log /usr/local/libexec/mariadbd \
              --user=mysql --datadir=/var/db/mysql --bind-address=0.0.0.0 --socket=/tmp/mysql.sock; }
stop_mdb() { j=$(mj); jexec "$j" sh -c 'mysqladmin --socket=/tmp/mysql.sock -u root shutdown 2>/dev/null; pkill -9 mariadbd 2>/dev/null'; }
set_addr() { r=$(rootfs); sed -i '' "s#wsrep_cluster_address .*=.*#wsrep_cluster_address     = $1#" \
              "$r/usr/local/etc/mysql/conf.d/galera.cnf" 2>/dev/null; }

# Wait for the supervisor to create the mariadb jail.
i=0; while [ -z "$(mj)" ] && [ "$i" -lt 40 ]; do sleep 3; i=$((i+1)); done
[ -z "$(mj)" ] && exit 0

# Already healthy? nothing to do.
[ "$(status)" = "Primary" ] && exit 0

if [ "$SELF" = "$BOOT" ]; then
	# Designated bootstrap node: give any surviving Primary a moment to
	# appear, then bring the Primary component up ourselves.
	sleep 15
	[ "$(status)" = "Primary" ] && exit 0
	stop_mdb; sleep 3
	set_addr "gcomm://"			# empty => bootstrap a new component
	r=$(rootfs)
	sed -i '' 's/safe_to_bootstrap: 0/safe_to_bootstrap: 1/' \
	    "$r/var/db/mysql/grastate.dat" 2>/dev/null
	start_mdb; sleep 10
	set_addr "$ADDR"			# restore full membership for next time
	logger -t galera-recover "bootstrapped Primary on $SELF"
else
	# Joiner: (re)start mariadbd until it joins the Primary. A joiner that
	# starts before the Primary exists will abort; we simply retry.
	set_addr "$ADDR"
	i=0
	while [ "$i" -lt 40 ]; do
		[ "$(status)" = "Primary" ] && { logger -t galera-recover "joined on $SELF"; exit 0; }
		mdb_up || start_mdb
		sleep 8; i=$((i+1))
	done
fi
