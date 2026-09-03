#!/bin/sh
# runs ON a cluster node; deploys the professional skin into its wp + nginx pods
WP=$(ocifbsd list 2>/dev/null | awk '/wordpress/{print $1}')
NG=$(ocifbsd list 2>/dev/null | awk '/nginx/{print $1}')
echo "wp=$WP"; echo "ng=$NG"
# CSS into BOTH pods (nginx serves it static; wp needs it for filemtime version)
ocifbsd exec "$WP" sh -c 'cat > /usr/local/www/wordpress/wp-content/mu-plugins/professional.css' < /tmp/professional.css
ocifbsd exec "$NG" sh -c 'cat > /usr/local/www/wordpress/wp-content/mu-plugins/professional.css' < /tmp/professional.css
# PHP loader into wp pod; disable old cyberpunk php
ocifbsd exec "$WP" sh -c 'cat > /usr/local/www/wordpress/wp-content/mu-plugins/oci-professional.php' < /tmp/oci-professional.php
ocifbsd exec "$WP" sh -c 'cd /usr/local/www/wordpress/wp-content/mu-plugins && [ -f oci-cyberpunk.php ] && mv -f oci-cyberpunk.php oci-cyberpunk.php.disabled; rm -rf /var/tmp/wpcache/* 2>/dev/null; ls | tr "\n" " "; echo'
echo "done-node"
