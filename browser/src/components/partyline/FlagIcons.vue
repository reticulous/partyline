<!-- A server's announce flags as icons: a lock for a server password, a
     checklist for an allow list. Nothing when neither is set. -->
<template>
  <span v-if="flags & (ANNOUNCE_PASSWORD | ANNOUNCE_ALLOWLIST)" class="fl">
    <q-icon v-if="flags & ANNOUNCE_PASSWORD" :name="matLock" size="13px" title="Needs a server password" />
    <q-icon v-if="flags & ANNOUNCE_ALLOWLIST" :name="matPlaylistAddCheck" size="14px"
            title="Only identities on its allow list may connect" />
  </span>
</template>

<script setup lang="ts">
import { matLock, matPlaylistAddCheck } from '@quasar/extras/material-icons'
import { ANNOUNCE_PASSWORD, ANNOUNCE_ALLOWLIST } from '../../modules/partyline'

defineProps<{ flags: number }>()
</script>

<style scoped>
.fl { display: inline-flex; gap: 2px; align-items: center; color: #c9a14a; flex: none; }
</style>
