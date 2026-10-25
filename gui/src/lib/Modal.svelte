<script lang="ts">
  // A dialog over the window: a title, a body, and a row of buttons.
  // Escape or a click outside it closes it (only the top one, when one
  // dialog comes up over another).
  import type { Snippet } from 'svelte';

  let { title, wide = false, onclose, children, actions }: {
    title: string;
    wide?: boolean;
    onclose: () => void;
    children: Snippet;
    actions?: Snippet;
  } = $props();

  let backdrop: HTMLDivElement;

  function key(e: KeyboardEvent) {
    const all = document.querySelectorAll('.backdrop');
    if (e.key === 'Escape' && all[all.length - 1] === backdrop) onclose();
  }
</script>

<svelte:window onkeydown={key} />

<!-- svelte-ignore a11y_click_events_have_key_events, a11y_no_static_element_interactions -->
<div class="backdrop" bind:this={backdrop} onclick={(e) => { if (e.target === e.currentTarget) onclose(); }}>
  <div class="modal" class:wide role="dialog" aria-modal="true" aria-label={title}>
    <h3>{title}</h3>
    <div class="content">{@render children()}</div>
    {#if actions}<div class="actions">{@render actions()}</div>{/if}
  </div>
</div>
