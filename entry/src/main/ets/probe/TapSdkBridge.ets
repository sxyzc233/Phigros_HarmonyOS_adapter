import probe from 'libtransprobe.so';
import { hilog } from '@kit.PerformanceAnalysisKit';

interface NativeRequest { id: number; command: string; }
export interface SdkCommand {
  service: string;
  method: string;
  args: string;
  callback: boolean;
  callbackId: string;
  onceTime: boolean;
}
interface SdkReply { code: number; message: string; content: string; callbackId: string; onceTime: boolean; }
export interface SdkCall { id: number; command: SdkCommand; }

/** Actual original Command/Result transport; no synthetic login/identity success. */
export class TapSdkBridge {
  private timer: number = -1;
  onCall: (call: SdkCall) => void = () => {};

  start(): void {
    if (this.timer >= 0) return;
    this.timer = setInterval(() => {
      const batch: NativeRequest[] = probe.bridgeRequests();
      for (const request of batch) {
        try {
          const command: SdkCommand = JSON.parse(request.command) as SdkCommand;
          // Whitelist the public routing fields. Arguments can contain credentials/identifiers.
          hilog.info(0, 'TransSdk', 'SDK_COMMAND service=%{public}s method=%{public}s callback=%{public}d argsKeys=%{public}s',
            command.service, command.method, command.callback ? 1 : 0,
            command.args.length > 0 ? Object.keys(JSON.parse(command.args) as Object).join(',') : '');
          this.onCall({ id: request.id, command: command });
        } catch (_) {
          hilog.error(0, 'TransSdk', 'SDK_COMMAND_INVALID id=%{public}d', request.id);
        }
      }
    }, 100);
  }

  stop(): void { if (this.timer >= 0) clearInterval(this.timer); this.timer = -1; }

  release(call: SdkCall): void { probe.releaseBridge(call.id); }

  reply(call: SdkCall, content: string, error: string = ''): boolean {
    if (!call.command.callback) return false;
    const result: SdkReply = { code: error.length === 0 ? 0 : -1, message: error.length === 0 ? 'Success' : error,
      content: content, callbackId: call.command.callbackId, onceTime: call.command.onceTime };
    return probe.replyBridge(call.id, JSON.stringify(result), call.command.onceTime);
  }
}
