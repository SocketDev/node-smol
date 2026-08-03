/**
 * Test constants for binject.
 *
 * Re-exports shared constants from build-infra that match the limits defined in
 * the C source code.
 */

export {
  MAX_NODE_BINARY_SIZE,
  MAX_VFS_SIZE,
} from 'build-infra/lib/constants'

/**
 * Maximum SEA blob size binject accepts.
 *
 * binject caps EVERY resource read at MAX_RESOURCE_SIZE (500 MB) in binject.c's
 * binject_read_resource(), so this — not build-infra's MAX_SEA_BLOB_SIZE
 * (2 GB - 1, Node's own kMaxPayloadSize ceiling) — is the limit the binary
 * actually enforces for SEA blobs.
 */
export const MAX_SEA_BLOB_SIZE = 500 * 1024 * 1024
