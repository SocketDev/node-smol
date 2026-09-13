declare module 'node:smol-ai' {
  export interface LanguageModelCreateOptions {
    readonly expectedInputs?: readonly LanguageModelModality[] | undefined
    readonly expectedOutputs?: readonly LanguageModelModality[] | undefined
    readonly maxTokens?: number | undefined
    monitor?(monitor: LanguageModelMonitor): void
    readonly seed?: number | undefined
    readonly signal?: AbortSignal | undefined
    readonly temperature?: number | undefined
    readonly threads?: number | undefined
    readonly topK?: number | undefined
  }

  export interface LanguageModelMessage {
    readonly content: string
    readonly role: 'assistant' | 'system' | 'user'
  }

  export interface LanguageModelModality {
    readonly languages?: readonly string[] | undefined
    readonly type: string
  }

  export interface LanguageModelMonitor {
    addEventListener(
      type: 'downloadprogress',
      listener: (event: { readonly loaded: number }) => void,
    ): void
  }

  export interface LanguageModelSession {
    readonly inputQuota: number
    readonly inputUsage: number
    readonly reproducibility: Readonly<{
      backend: string
      model: string
      modelSha256: string
      seed: number
      temperature: number
      threads: number
      topK: number
    }>
    clone(): Promise<LanguageModelSession>
    destroy(): void
    measureInputUsage(input: LanguageModelPrompt): Promise<number>
    prompt(
      input: LanguageModelPrompt,
      options?: { readonly signal?: AbortSignal | undefined } | undefined,
    ): Promise<string>
    promptStreaming(
      input: LanguageModelPrompt,
      options?: { readonly signal?: AbortSignal | undefined } | undefined,
    ): ReadableStream<string>
  }

  export type LanguageModelPrompt =
    | string
    | LanguageModelMessage
    | readonly LanguageModelMessage[]

  export const LanguageModel: Readonly<{
    availability(): Promise<
      'available' | 'downloadable' | 'downloading' | 'unavailable'
    >
    readonly capabilities: Readonly<{
      deterministicSeed: true
      text: true
      tools: false
      vision: false
    }>
    create(options?: LanguageModelCreateOptions | undefined): Promise<LanguageModelSession>
    params(): Promise<Readonly<{
      defaultTemperature: number
      defaultTopK: number
      maxTemperature: number
      maxTopK: number
    }>>
  }>
}
