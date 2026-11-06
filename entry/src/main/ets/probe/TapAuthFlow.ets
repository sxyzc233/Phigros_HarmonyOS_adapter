import probe from 'libtransprobe.so';
import { http } from '@kit.NetworkKit';
import { cryptoFramework } from '@kit.CryptoArchitectureKit';
import { util } from '@kit.ArkTS';
import { hilog } from '@kit.PerformanceAnalysisKit';
import { TapSdkBridge, SdkCall } from './TapSdkBridge';

interface LoginInit { clientID?: string; }
interface AntiInit { gameIdentifier?: string; useAgeRange?: boolean; }
interface Startup { startupWithTapTap?: string; userIdentifier?: string; }
interface DeviceCode {
  device_code?: string; user_code?: string; verification_uri_complete?: string; qrcode_url?: string;
  verification_url?: string; verification_uri?: string; expires_in?: number; interval?: number;
}
export interface AccessToken { kid?: string; mac_key?: string; access_token?: string; scope?: string; error?: string;
  token_type?: string; mac_algorithm?: string; }
interface DeviceEnvelope { data?: DeviceCode; error?: string; }
interface TokenEnvelope { data?: AccessToken; error?: string; }
interface Identify { status?: string; anti_addiction_token?: string; ageLimit?: number; }
interface IdentifyEnvelope extends Identify { data?: Identify; }
interface RealName { age_limit?: number; is_adult?: boolean; }
interface Rating { allow?: boolean; }
interface Policy { policy_active?: string; policy_model?: string; policy_heartbeat_interval?: number; }
interface PolicyConfig { real_name?: RealName; content_rating_check?: Rating; anti_addiction?: Policy; }
interface PolicyEnvelope extends PolicyConfig { data?: PolicyConfig; }
interface Heartbeat { remain_time?: number; title?: string; description?: string; }
interface HeartbeatEnvelope extends Heartbeat { data?: Heartbeat; }
interface ServerEnvelope { success?: boolean; now?: number; }
interface ServerFailure { error?: string; code?: number; error_description?: string; message?: string; }
interface FailureEnvelope extends ServerFailure { data?: ServerFailure; }
interface AuthExtras { description: string; remaining_time: number; }
interface AuthEvent { code: number; extras: AuthExtras; }

export interface TapAuthView { visible: boolean; message: string; qr: string; busy: boolean; }
const AUTH_HOST: string = 'https://accounts.tapapis.cn';
const ANTI_HOST: string = 'https://tds-tapsdk.cn.tapapis.com';

/** Host adapter for original Android SDK protocol. No success without verified real server responses. */
export class TapAuthFlow {
  onView: (view: TapAuthView) => void = () => {};
  private sdk: TapSdkBridge;
  private callback?: SdkCall;
  private clientId: string = '';
  private userId: string = '';
  private useAgeRange: boolean = true;
  private running: boolean = false;
  private generation: number = 0;
  private oauth?: AccessToken;
  private antiToken: string = '';
  private age: number = -1;
  private remaining: number = 0;
  private heartbeatTimer: number = -1;
  private countdownTimer: number = -1;
  private validUntil: number = 0;
  private sessionId: string = '';
  private offset: number = 0;
  private view: TapAuthView = { visible: false, message: '', qr: '', busy: false };

  constructor(sdk: TapSdkBridge) { this.sdk = sdk; }

  private display(message: string, qr: string = '', busy: boolean = false): void {
    this.view = { visible: true, message: message, qr: qr, busy: busy }; this.onView(this.view);
  }

  handle(call: SdkCall): void {
    const c = call.command;
    if (c.service === 'TDSLoginService' && c.method === 'init') {
      const args: LoginInit = JSON.parse(c.args) as LoginInit; this.clientId = args.clientID ?? ''; return;
    }
    if (c.service !== 'TapAntiAddictionService') {
      if (c.callback) this.sdk.reply(call, '', 'This SDK service is not implemented by the Harmony host');
      return;
    }
    if (c.method === 'init') {
      const args: AntiInit = JSON.parse(c.args) as AntiInit;
      this.clientId = args.gameIdentifier ?? this.clientId; this.useAgeRange = args.useAgeRange ?? false;
    } else if (c.method === 'setAntiAddictionCallback') {
      this.callback = call;
      hilog.info(0, 'TransSdk', 'AUTH_CALLBACK_REGISTERED persistent=%{public}d', c.onceTime ? 0 : 1);
    } else if (c.method === 'startupWithTapTap' || c.method === 'startup') {
      const args: Startup = JSON.parse(c.args) as Startup; this.userId = args.startupWithTapTap ?? args.userIdentifier ?? '';
      this.display('原游戏需要 TapTap 登录及实名／防沉迷认证。点击下方按钮，在 TapTap 官方页面授权。');
    } else if (c.method === 'getCurrentToken') {
      this.sdk.reply(call, JSON.stringify({ token: this.antiToken }));
    } else if (c.method === 'getCurrentUserAgeLimit') {
      // This legacy Java endpoint is deprecated and returns -1 even in the original SDK.
      this.sdk.reply(call, JSON.stringify({ ageLimit: -1 }));
    } else if (c.method === 'getCurrentUserRemainTime') {
      this.sdk.reply(call, JSON.stringify({ remainTime: this.remaining }));
    } else if (c.method === 'logout') {
      this.cancel();
    } else if (c.method !== 'setUnityVersion' && c.method !== 'enterGame' && c.method !== 'leaveGame') {
      if (c.callback) this.sdk.reply(call, '', 'This anti-addiction method is not implemented by the Harmony host');
    }
  }

  private emit(code: number, description: string): void {
    const event: AuthEvent = { code: code, extras: { description: description, remaining_time: this.remaining } };
    if (this.callback) this.sdk.reply(this.callback, JSON.stringify(event));
    hilog.info(0, 'TransSdk', 'AUTH_EVENT code=%{public}d callback=%{public}d', code, this.callback ? 1 : 0);
  }

  private clearIdentity(): void {
    this.oauth = undefined; this.antiToken = ''; this.age = -1; this.remaining = 0;
    probe.setSdkAuthState(-1, 0, '', false);
    if (this.heartbeatTimer >= 0) clearInterval(this.heartbeatTimer); this.heartbeatTimer = -1;
    if (this.countdownTimer >= 0) clearInterval(this.countdownTimer); this.countdownTimer = -1;
  }

  cancel(): void {
    ++this.generation; this.running = false; this.clearIdentity(); this.emit(1000, '用户取消认证');
    this.view = { visible: false, message: '', qr: '', busy: false }; this.onView(this.view);
  }
  stop(): void { ++this.generation; this.running = false; this.clearIdentity(); }

  authorizationUrl(): string { return this.view.qr; }
  private validUrl(value: string): boolean {
    return /^https:\/\/([a-z0-9-]+\.)*(taptap\.cn|taptap\.com|taptap\.io|tapapis\.cn|tapapis\.com)(:443)?(\/|\?|$)/i.test(value);
  }
  form(pairs: string[]): string {
    let values: string[] = [];
    for (let i: number = 0; i + 1 < pairs.length; i += 2) values.push(encodeURIComponent(pairs[i]) + '=' + encodeURIComponent(pairs[i + 1]));
    return values.join('&');
  }
  private random(): string {
    return new util.Base64Helper().encodeToStringSync(cryptoFramework.createRandom().generateRandomSync(24).data)
      .replace(/[^a-zA-Z0-9]/g, '');
  }
  async request(stage: string, endpoint: string, body: string = '', authorization: string = '',
    antiToken: string = ''): Promise<string> {
    const client = http.createHttp();
    const headers: Record<string, string> = { 'Accept': 'application/json', 'accept-language': 'zh-cn',
      'User-Agent': stage === 'IDENTIFY' ? 'AntiAddictionAndroidSDK/3.30.3 HarmonyHost' : 'Version=3.30.3&Platform=Android' };
    if (body.length > 0) headers['Content-Type'] = stage === 'HEARTBEAT' ? 'application/json;charset=UTF-8' : 'application/x-www-form-urlencoded';
    if (authorization.length > 0) headers['Authorization'] = authorization;
    if (antiToken.length > 0) headers['X-TAP-Anti-Addiction-Token'] = antiToken;
    try {
      const options: http.HttpRequestOptions = { method: body.length > 0 ? http.RequestMethod.POST : http.RequestMethod.GET,
        header: headers, expectDataType: http.HttpDataType.STRING, connectTimeout: 15000, readTimeout: 20000, usingCache: false };
      // Empty extraData is NOT equivalent to absent: on GET the platform appends '&' to an
      // existing query. Keep the transmitted URI byte-for-byte identical to the MAC input.
      if (body.length > 0) options.extraData = body;
      const response = await client.request(endpoint, options);
      hilog.info(0, 'TransSdk', 'AUTH_HTTP stage=%{public}s status=%{public}d', stage, response.responseCode);
      const dates: Record<string, string> = response.header as Record<string, string>;
      const date: number = Date.parse(dates['date'] ?? dates['Date'] ?? '');
      if (Number.isFinite(date)) this.offset = date - Date.now();
      const data: string = response.result as string;
      if (typeof data !== 'string' || data.length > 1048576) throw new Error(stage + ': 响应格式不支持');
      if (response.responseCode < 200 || response.responseCode >= 300) {
        if (stage.endsWith('TOKEN') && (response.responseCode === 400 || response.responseCode === 403)) return data;
        let reason: string = 'unknown'; let code: number = 0; let kind: string = 'unknown';
        try {
          const parsed: FailureEnvelope = JSON.parse(data) as FailureEnvelope;
          const failure: ServerFailure = parsed.data ?? parsed;
          if (typeof failure.error === 'string' && /^[a-z][a-z0-9_]{0,63}$/.test(failure.error)) kind = failure.error;
          if (typeof failure.code === 'number' && Number.isFinite(failure.code)) code = failure.code;
          // Classify known terms without copying response text, URLs, IDs or signatures into logs.
          const description: string = (failure.error_description ?? '') + ' ' + (failure.message ?? '') + ' ' + kind;
          if (/scope|permission|权限/i.test(description)) reason = 'scope';
          else if (/time|timestamp|时间/i.test(description)) reason = 'time';
          else if (/signature|sign|mac|签名/i.test(description)) reason = 'signature';
          else if (/client|application|应用/i.test(description)) reason = 'client';
          else if (/token|expired|凭证/i.test(description)) reason = 'token';
        } catch (_) {}
        hilog.error(0, 'TransSdk', 'AUTH_SERVER_REJECT stage=%{public}s status=%{public}d kind=%{public}s code=%{public}d reason=%{public}s',
          stage, response.responseCode, kind, code, reason);
        throw new Error(stage + ': 服务器返回 ' + response.responseCode + '（' + kind + ' / ' + code + ' / ' + reason + '）');
      }
      if (stage === 'IDENTIFY' || stage === 'POLICY' || stage === 'HEARTBEAT') {
        const status: ServerEnvelope = JSON.parse(data) as ServerEnvelope;
        if (status.success !== true) throw new Error(stage + ': 服务未确认此次请求成功');
        if (typeof status.now === 'number' && status.now > 0) this.offset = status.now * 1000 - Date.now();
      }
      return data;
    } finally { client.destroy(); }
  }

  async mac(endpoint: string, token: AccessToken): Promise<string> {
    // OAuth credential identifiers are opaque, and may include base64 punctuation.
    // Preserve the identifier and escape the HTTP quoted-string; forbid header controls.
    if (typeof token.kid !== 'string' || typeof token.mac_key !== 'string' || token.kid.length === 0 ||
      token.kid.length > 4096 || token.mac_key.length === 0 || token.mac_key.length > 4096 || /[^\x20-\x7E]/.test(token.kid)) {
      throw new Error('登录凭证格式不支持');
    }
    const keyId: string = token.kid.replace(/\\/g, '\\\\').replace(/"/g, '\\"');
    const slash: number = endpoint.indexOf('/', 8); const host: string = endpoint.substring(8, slash);
    const path: string = endpoint.substring(slash); const nonce: string = this.random();
    const ts: string = Math.floor((Date.now() + this.offset) / 1000).toString();
    const signed: string = await this.hmac(ts + '\n' + nonce + '\nGET\n' + path + '\n' + host + '\n443\n\n', token.mac_key);
    return 'MAC id="' + keyId + '",ts="' + ts + '",nonce="' + nonce + '",mac="' + signed + '"';
  }

  private async hmac(text: string, secret: string): Promise<string> {
    const encoder = new util.TextEncoder();
    const key = await cryptoFramework.createSymKeyGenerator('HMAC').convertKey({ data: encoder.encodeInto(secret) });
    const mac = cryptoFramework.createMac('SHA1'); await mac.init(key);
    await mac.update({ data: encoder.encodeInto(text) });
    return new util.Base64Helper().encodeToStringSync((await mac.doFinal()).data);
  }

  async begin(): Promise<void> {
    if (this.running) return;
    if (!this.clientId || !this.userId || !this.callback) { this.display('原包认证参数或回调尚未就绪，请重新启动后再试。'); return; }
    this.running = true; const generation: number = ++this.generation; this.clearIdentity();
    try {
      // RFC 2202 case 2 checks the real device's crypto backend before sending any identity request.
      if (await this.hmac('what do ya want for nothing?', 'Jefe') !== '7/zfauXrL6LSdBbV8YTfnCWafHk=') throw new Error('设备签名运算检查未通过');
      hilog.info(0, 'TransSdk', 'AUTH_HMAC_SELFTEST PASS');
      this.display('正在连接 TapTap 官方授权服务…', '', true);
      const envelope: DeviceEnvelope = JSON.parse(await this.request('DEVICE', AUTH_HOST + '/oauth2/v1/device/code',
        this.form(['client_id', this.clientId, 'response_type', 'device_code', 'scope', this.useAgeRange ? 'compliance' : 'compliance_basic']))) as DeviceEnvelope;
      if (generation !== this.generation) return;
      const device: DeviceCode | undefined = envelope.data;
      if (!device?.device_code) throw new Error('TapTap 未提供有效授权码，当前应用或权限范围可能不支持此授权方式');
      const link: string = device.verification_uri_complete ?? device.qrcode_url ?? device.verification_url ?? device.verification_uri ?? '';
      if (!this.validUrl(link)) throw new Error('TapTap 授权地址未通过验证');
      this.display('请使用 TapTap 扫码，或打开官方授权页面完成登录与实名授权。' + (device.user_code ? '\n授权码：' + device.user_code : ''), link, true);
      const deadline: number = Date.now() + Math.min(Math.max(device.expires_in ?? 300, 30), 900) * 1000;
      let interval: number = Math.min(Math.max(device.interval ?? 5, 3), 30);
      while (Date.now() < deadline && generation === this.generation) {
        await new Promise<void>((resolve) => setTimeout(resolve, interval * 1000));
        if (generation !== this.generation) return;
        const response: TokenEnvelope = JSON.parse(await this.request('TOKEN', AUTH_HOST + '/oauth2/v1/token',
          this.form(['grant_type', 'device_token', 'client_id', this.clientId, 'secret_type', 'hmac-sha-1', 'code', device.device_code]))) as TokenEnvelope;
        if (generation !== this.generation) return;
        const token: AccessToken | undefined = response.data;
        const error: string = token?.error ?? response.error ?? '';
        if (error === 'authorization_pending' || error === 'authorization_waiting') continue;
        if (error === 'slow_down') { interval = Math.min(interval + 5, 30); continue; }
        if (error) throw new Error('TapTap 授权被取消或已过期，请重试');
        if (!token?.kid || !token.mac_key || !token.access_token) throw new Error('TapTap 返回的登录凭证不完整');
        const scopes: string[] = typeof token.scope === 'string' ? token.scope.split(/[\s,]+/) : [];
        const hasScope: boolean = scopes.includes('compliance') || (!this.useAgeRange && scopes.includes('compliance_basic'));
        hilog.info(0, 'TransSdk', 'AUTH_SCOPE_CHECK pass=%{public}d fieldString=%{public}d ageRange=%{public}d',
          hasScope ? 1 : 0, typeof token.scope === 'string' ? 1 : 0, this.useAgeRange ? 1 : 0);
        if (!hasScope) throw new Error('此次设备授权未授予实名权限，需改接原包的网页授权流程');
        this.oauth = token; break;
      }
      if (generation !== this.generation) return;
      if (!this.oauth) throw new Error('授权码已过期，请重试');
      this.display('已获得授权，正在向实名服务核验状态…', '', true);
      const identifyUrl: string = ANTI_HOST + '/real-name/v2/anti-addiction-token-taptap?' +
        this.form(['client_id', this.clientId, 'user_identifier', this.userId]);
      const identified: IdentifyEnvelope = JSON.parse(await this.request('IDENTIFY', identifyUrl, '',
        await this.mac(identifyUrl, this.oauth))) as IdentifyEnvelope;
      if (generation !== this.generation) return;
      const identity: Identify = identified.data ?? identified;
      if (identity.status !== 'pass' || !identity.anti_addiction_token) {
        throw new Error(identity.status === 'waiting' ? '实名审核尚未完成，请在 TapTap 官方页面查看状态后重试' : '该账号尚未通过实名核验，请先在 TapTap 完成认证');
      }
      this.antiToken = identity.anti_addiction_token; this.oauth = undefined;
      const configUrl: string = ANTI_HOST + '/anti-addiction/v2/get-config-by-token?platform=android&' +
        this.form(['client_id', this.clientId, 'user_identifier', this.userId]);
      const envelopeConfig: PolicyEnvelope = JSON.parse(await this.request('POLICY', configUrl, '', '', this.antiToken)) as PolicyEnvelope;
      if (generation !== this.generation) return;
      const config: PolicyConfig = envelopeConfig.data ?? envelopeConfig;
      if (config.content_rating_check?.allow !== true || !config.real_name || !config.anti_addiction) throw new Error('服务未允许游玩，或防沉迷配置暂不支持');
      // The server supplies the age range. Unknown ranges remain -1, never guessed as adult.
      const age: number = config.real_name.age_limit ?? -1;
      this.age = this.useAgeRange && age >= 0 && age <= 18 ? age : -1;
      this.sessionId = this.random();
      await this.checkPlay(generation);
      if (generation !== this.generation) return;
      probe.setSdkAuthState(this.age, this.remaining, this.antiToken, true);
      this.emit(500, '实名及游玩权限已由服务器确认');
      this.view = { visible: false, message: '', qr: '', busy: false }; this.onView(this.view);
      const seconds: number = Math.min(Math.max(config.anti_addiction.policy_heartbeat_interval ?? 60, 5), 300);
      this.heartbeatTimer = setInterval(() => {
        this.checkPlay(generation).then(() => {
          if (generation === this.generation) probe.setSdkAuthState(this.age, this.remaining, this.antiToken, true);
        }).catch(() => {
          if (generation !== this.generation) return;
          this.clearIdentity(); this.emit(1200, '无法确认游玩时限，请重新认证'); this.display('游玩权限检查中断，请重新认证。');
        });
      }, seconds * 1000);
      this.countdownTimer = setInterval(() => {
        if (generation !== this.generation) return;
        this.remaining = Math.max(0, Math.floor((this.validUntil - Date.now()) / 1000));
        if (this.remaining <= 0) {
          this.clearIdentity(); this.emit(1050, '服务器允许的游玩时长已用尽'); this.display('游玩时长已用尽，请按 TapTap 的防沉迷提示安排时间。');
        } else probe.setSdkAuthState(this.age, this.remaining, this.antiToken, true);
      }, 1000);
    } catch (error) {
      if (generation !== this.generation) return;
      // Only generated messages are shown; raw network responses, tokens, names and IDs are withheld.
      const message: string = error instanceof Error ? error.message : '连接或认证失败，请重试';
      this.clearIdentity(); this.display(message);
      hilog.error(0, 'TransSdk', 'AUTH_FAILED (credentials and response withheld)');
    } finally { if (generation === this.generation) this.running = false; }
  }

  private async checkPlay(generation: number): Promise<void> {
    const endpoint: string = ANTI_HOST + '/anti-addiction/v2/heartbeat?' + this.form(['client_id', this.clientId, 'user_identifier', this.userId]);
    const envelope: HeartbeatEnvelope = JSON.parse(await this.request('HEARTBEAT', endpoint,
      JSON.stringify({ session_id: this.sessionId }), '', this.antiToken)) as HeartbeatEnvelope;
    if (generation !== this.generation) return;
    const result: Heartbeat = envelope.data ?? envelope;
    if (typeof result.remain_time !== 'number' || result.remain_time <= 0) throw new Error('当前时段或剩余时长不允许游玩，请在 TapTap 查看防沉迷提示');
    this.remaining = Math.floor(result.remain_time);
    this.validUntil = Date.now() + this.remaining * 1000;
  }
}
