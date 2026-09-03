<?php
/**
 * Plugin Name: CloudBSD OCI — Professional Skin
 * Description: Enterprise dark CloudBSD/RevyTech brand skin for the OCI showcase
 *              sites, replacing the earlier cyberpunk theme. Adds the node badge,
 *              cross-site switcher, and the required local-time + REVYTECH footer.
 * Author: Mark LaPointe <mark@revytechinc.com> — REVYTECH, Inc.
 * Version: 1.0
 */
if (!defined('ABSPATH')) exit;

/* Which showcase are we? cluster vs single, by site URL. */
function ocipro_is_cluster() {
    return (strpos(home_url(), 'ocisingle') === false);
}

/* Fonts + the skin stylesheet. Retire the old cyberpunk sheet if present. */
add_action('wp_enqueue_scripts', function () {
    wp_dequeue_style('cyberpunk-css');
    wp_deregister_style('cyberpunk-css');
    wp_enqueue_style(
        'oci-fonts',
        'https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600&family=Outfit:wght@400;600;700;800&display=swap',
        [], null
    );
    $cssfile = plugin_dir_path(__FILE__) . 'professional.css';
    $cssver  = file_exists($cssfile) ? (string) filemtime($cssfile) : '1.0';
    wp_enqueue_style(
        'oci-professional',
        plugins_url('professional.css', __FILE__),
        ['oci-fonts'],
        $cssver
    );
}, 20);

/* Skin body classes. */
add_filter('body_class', function ($classes) {
    $classes[] = 'oci-pro-skin';
    $classes[] = ocipro_is_cluster() ? 'oci-cluster' : 'oci-single';
    return $classes;
});

/* Top ribbon: architecture badge + cross-site switcher. */
add_action('wp_body_open', function () {
    $cluster = ocipro_is_cluster();
    if ($cluster) {
        $badge = '<span class="oci-badge"><span class="oci-dot"></span><span class="oci-dot"></span><span class="oci-dot"></span> 3-Node HA Cluster</span>';
        $switch = 'Viewing the three-node high-availability cluster. <span class="oci-switch">Compare the <a href="https://ocisingle.cloudbsd.org/">single-node baseline &rarr;</a></span>';
    } else {
        $badge = '<span class="oci-badge"><span class="oci-dot"></span> Standalone Node</span>';
        $switch = 'Viewing the single-instance baseline. <span class="oci-switch">Compare the <a href="https://ocicluster.cloudbsd.org/">3-node HA cluster &rarr;</a></span>';
    }
    echo '<div class="oci-ribbon">' . $badge . '<span class="oci-switch-wrap">' . $switch . '</span></div>';
});

/* Footer: viewer-local-time "last updated" + REVYTECH / Mark LaPointe attribution.
   The timestamp is rendered client-side from an embedded epoch so every visitor
   sees it in their own timezone. */
add_action('wp_footer', function () {
    $epoch = time();
    ?>
<div class="oci-attrib">
  <div>Last updated <span class="oci-updated" id="oci-updated" data-epoch="<?php echo (int) $epoch; ?>">&hellip;</span></div>
  <div>Content &amp; design are the property of <strong>REVYTECH,&nbsp;Inc.</strong> &mdash; authored by <strong>Mark&nbsp;LaPointe</strong> &lt;<a href="mailto:mark@revytechinc.com">mark@revytechinc.com</a>&gt;.</div>
  <div style="margin-top:.35rem;color:#475569;">Served by <strong>ocifbsd</strong> &mdash; a FreeBSD-native OCI runtime (jails&nbsp;+&nbsp;VNET), fronted by the native ocifbsd L4 proxy.</div>
</div>
<script>
(function(){
  var el = document.getElementById('oci-updated');
  if (!el) return;
  var ms = parseInt(el.getAttribute('data-epoch'), 10) * 1000;
  try {
    el.textContent = new Date(ms).toLocaleString(undefined, {
      year:'numeric', month:'short', day:'numeric',
      hour:'2-digit', minute:'2-digit', timeZoneName:'short'
    });
  } catch (e) {
    el.textContent = new Date(ms).toLocaleString();
  }
})();
</script>
    <?php
}, 99);
