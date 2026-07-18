/**
 * @file Authoritative contracts for Rust-canonical capabilities consumed by
 * node-smol. A pin advances only together with its adapter and fixture proof.
 */

export interface UpstreamContract {
  readonly addonCrate: string
  readonly adapter: 'addon-first'
  readonly builtin?: string | undefined
  readonly crate: string
  readonly fixture?: string | undefined
  readonly name: string
  readonly revision: string
  readonly schemaVersion: 1
  readonly targets: readonly string[]
}

export const UPSTREAM_CONTRACTS: readonly UpstreamContract[] = [
  {
    addonCrate: 'napi/decmpfs',
    adapter: 'addon-first',
    crate: 'crates/decmpfs',
    name: 'decmpfs',
    revision: '3f173ff2df6443d850c5f3ed10207256316e689b',
    schemaVersion: 1,
    targets: [
      'aarch64-apple-darwin',
      'x86_64-apple-darwin',
      'aarch64-unknown-linux-gnu',
      'x86_64-unknown-linux-gnu',
      'aarch64-pc-windows-msvc',
      'x86_64-pc-windows-msvc',
    ],
  },
  {
    addonCrate: 'napi/stuie',
    adapter: 'addon-first',
    builtin: 'node:smol-tui',
    crate: 'crates/stuie',
    fixture: 'fixtures/mouse-events.golden.json',
    name: 'stuie',
    revision: '6ccd9166fb9f892aeeaf0642c56bbe4cb648cc97',
    schemaVersion: 1,
    targets: [
      'aarch64-apple-darwin',
      'x86_64-apple-darwin',
      'aarch64-unknown-linux-gnu',
      'x86_64-unknown-linux-gnu',
      'aarch64-pc-windows-msvc',
      'x86_64-pc-windows-msvc',
    ],
  },
]
