<script lang="ts" module>
  export type DiskKind = 'floppy525' | 'floppy35' | 'hard';

  // Which picture fits an image of this many bytes. A 5.25" disk is 140K
  // (160K for the rare 40 track ones), a 3.5" is 400K or 800K, and anything
  // bigger is a hard drive.
  export function diskKind(bytes: number): DiskKind {
    if (bytes <= 160 * 1024) return 'floppy525';
    if (bytes <= 800 * 1024) return 'floppy35';
    return 'hard';
  }
</script>

<script lang="ts">
  // A drawing of a disk, in the app's colors. The disk is in a 120 by 120
  // box; the arrow, if there is one, sits above it.
  let { kind, arrow = false, class: cls = '' }: {
    kind: DiskKind;
    arrow?: boolean;
    class?: string;
  } = $props();
</script>

<svg class="disk-art {cls}" viewBox={arrow ? '0 -36 120 156' : '0 0 120 120'} aria-hidden="true">
  {#if arrow}
    <g class="arrow">
      <line x1="60" y1="-32" x2="60" y2="-6" />
      <polyline points="50,-15 60,-5 70,-15" />
    </g>
  {/if}
  {#if kind === 'floppy525'}
    <!-- Label, write-protect notch, hub, index hole and the read slot -->
    <path class="body" d="M7 4 H113 A3 3 0 0 1 116 7 V18 H111 V28 H116 V113 A3 3 0 0 1 113 116 H7 A3 3 0 0 1 4 113 V7 A3 3 0 0 1 7 4 Z" />
    <rect class="label" x="14" y="10" width="88" height="16" rx="1" />
    <circle class="ring" cx="60" cy="54" r="20" />
    <circle class="hole" cx="60" cy="54" r="13" />
    <circle class="hole" cx="83" cy="62" r="2.5" />
    <rect class="hole" x="54" y="81" width="12" height="29" rx="6" />
  {:else if kind === 'floppy35'}
    <!-- The cut corner, the metal shutter with its window, the label and
         the write-protect hole -->
    <path class="body" d="M8 6 H106 L116 16 V112 A4 4 0 0 1 112 116 H8 A4 4 0 0 1 4 112 V10 A4 4 0 0 1 8 6 Z" />
    <rect class="metal" x="32" y="6" width="58" height="38" rx="1" />
    <rect class="hole" x="64" y="12" width="14" height="26" rx="1" />
    <path class="label" d="M20 116 V60 A3 3 0 0 1 23 57 H97 A3 3 0 0 1 100 60 V116" />
    <rect class="hole" x="8" y="104" width="7" height="7" />
  {:else}
    <!-- An external drive box from the front: the case, a name plate,
         the vents, the bezel strip and its activity light -->
    <rect class="body" x="4" y="28" width="112" height="64" rx="5" />
    <rect class="label" x="44" y="40" width="60" height="16" rx="1" />
    <line class="ring" x1="16" y1="40" x2="16" y2="64" />
    <line class="ring" x1="24" y1="40" x2="24" y2="64" />
    <line class="ring" x1="32" y1="40" x2="32" y2="64" />
    <line class="ring" x1="4" y1="74" x2="116" y2="74" />
    <circle class="light" cx="104" cy="83" r="3" />
  {/if}
</svg>

<style>
  .disk-art * { stroke-width: 2; stroke-linejoin: round; stroke-linecap: round; }
  .body { fill: var(--panel); stroke: var(--muted); }
  .label { fill: var(--bg); stroke: var(--border); }
  .ring { fill: none; stroke: var(--border); }
  .hole { fill: var(--bg); stroke: var(--muted); }
  .metal { fill: var(--border); stroke: var(--muted); }
  .light { fill: var(--accent); stroke: none; }
  .arrow { fill: none; stroke: var(--accent); transition: transform 0.2s; }
  @media (prefers-reduced-motion: reduce) { .arrow { transition: none; } }
</style>
