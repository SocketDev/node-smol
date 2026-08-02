/**
 * Get checkpoint chain for curl CI workflows.
 *
 * Usage:
 * node scripts/get-checkpoint-chain.mts [--dev|--prod]
 *
 * Output:
 * Comma-separated checkpoint chain (e.g., "finalized,mbedtls")
 */

import { getDefaultLogger } from '@socketsecurity/lib-stable/logger/default'
import { getCheckpointChain } from './build.mts'

const logger = getDefaultLogger()

// Get the checkpoint chain. It is the same for dev and prod.
const chain = getCheckpointChain()

// Output as comma-separated string (for CI).
logger.log(chain.join(','))
