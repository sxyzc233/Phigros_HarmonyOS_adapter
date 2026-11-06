import { hilog } from '@kit.PerformanceAnalysisKit';
import { TapSdkBridge, SdkCall } from './TapSdkBridge';
import { TapAuthFlow, TapAuthView, AccessToken } from './TapAuthFlow';

interface InitArgs { clientID?: string; regionType?: boolean; }
interface LoginArgs { permissions?: string[]; }
interface PermissionArgs { permission?: string; appendPermission?: string; }
interface LoginWrapper { wrapper: string; loginCallbackCode: number; }
interface LoginDevice { device_code?: string; user_code?: string; verification_uri_complete?: string; qrcode_url?: string;
  verification_url?: string; verification_uri?: string; expires_in?: number; interval?: number; }
interface DeviceEnvelope { success?: boolean; data?: LoginDevice; }
interface TokenEnvelope { success?: boolean; data?: AccessToken; error?: string; }
interface TapProfile { openid?: string; unionid?: string; name?: string; avatar?: string; }
interface ProfileEnvelope { success?: boolean; data?: TapProfile; }

/** TDSLoginService adapter; only actual OAuth/token/profile results go to original LoginWrapperBean. */
export class TapLoginFlow {
  onView: (view: TapAuthView) => void = () => {};
  private sdk: TapSdkBridge;
  private network: TapAuthFlow;
  private clientId: string = '';
  private china: boolean = true;
  private callback?: SdkCall;
  private token?: AccessToken;
  private profile?: TapProfile;
  private permissions: string[] = ['public_profile'];
  private appended: string[] = [];
  private generation: number = 0;
  private running: boolean = false;
  private link: string = '';

  constructor(sdk: TapSdkBridge, network: TapAuthFlow) { this.sdk = sdk; this.network = network; }
  private display(message: string, qr: string = '', busy: boolean = false): void {
    this.link = qr; this.onView({ visible: true, message: message, qr: qr, busy: busy });
  }
  private hide(): void { this.link = ''; this.onView({ visible: false, message: '', qr: '', busy: false }); }
  authorizationUrl(): string { return this.link; }
  private emit(code: number, wrapper: string = ''): void {
    if (!this.callback) return;
    const result: LoginWrapper = { loginCallbackCode: code, wrapper: wrapper };
    this.sdk.reply(this.callback, JSON.stringify(result));
    hilog.info(0, 'TransSdk', 'LOGIN_EVENT code=%{public}d (credentials withheld)', code);
  }

  handle(call: SdkCall): void {
    const command = call.command;
    if (command.method === 'init' || command.method === 'initWithClientID') {
      const args: InitArgs = JSON.parse(command.args) as InitArgs;
      const clientId: string = args.clientID ?? '';
      if (clientId !== this.clientId) { this.token = undefined; this.profile = undefined; }
      this.clientId = clientId; this.china = args.regionType ?? true;
    } else if (command.method === 'registerLoginCallback') {
      if (this.callback) this.sdk.release(this.callback);
      this.callback = call;
      hilog.info(0, 'TransSdk', 'LOGIN_CALLBACK_REGISTERED');
    } else if (command.method === 'unregisterLoginCallback') {
      if (this.callback) this.sdk.release(this.callback); this.callback = undefined;
    } else if (command.method === 'startTapLogin') {
      const args: LoginArgs = JSON.parse(command.args) as LoginArgs;
      this.permissions = args.permissions?.length ? args.permissions : ['public_profile'];
      if (!this.china) { this.display('当前测试版尚未接入国际区账号登录。'); return; }
      if (this.token && this.profile && this.hasScopes(this.token)) {
        this.emit(0, JSON.stringify(this.token)); return;
      }
      this.display('云存档需要单独的 TapTap 账号授权。请在官方页面完成登录，游戏随后读取你的账号资料。');
    } else if (command.method === 'currentAccessToken') {
      this.sdk.reply(call, this.token ? JSON.stringify(this.token) : '');
    } else if (command.method === 'currentProfile') {
      this.sdk.reply(call, this.profile ? JSON.stringify(this.profile) : '');
    } else if (command.method === 'fetchProfileForCurrentAccessToken') {
      this.fetchProfile().then(profile => {
        this.sdk.reply(call, JSON.stringify({ loginCallbackCode: 0, wrapper: JSON.stringify(profile) }));
      }).catch(() => {
        this.sdk.reply(call, JSON.stringify({ loginCallbackCode: 1, wrapper: '账号资料读取失败，请重新登录' }));
      });
    } else if (command.method === 'logout') {
      ++this.generation; this.running = false; this.token = undefined; this.profile = undefined; this.hide();
    } else if (command.method === 'appendPermission') {
      const args: PermissionArgs = JSON.parse(command.args) as PermissionArgs;
      const value: string = args.permission ?? args.appendPermission ?? '';
      if (/^[a-z][a-z0-9_]{0,63}$/.test(value) && !this.appended.includes(value)) this.appended.push(value);
    } else if (command.method !== 'changeConfig' && command.callback) {
      this.sdk.reply(call, '', 'This TapTap login method is not implemented by the Harmony host');
    }
  }

  private scopes(): string[] {
    let result: string[] = [];
    for (const scope of this.permissions.concat(this.appended)) {
      if (!/^[a-z][a-z0-9_]{0,63}$/.test(scope)) throw new Error('原包请求的授权范围格式不支持');
      if (!result.includes(scope)) result.push(scope);
    }
    return result;
  }
  private hasScopes(token: AccessToken): boolean {
    const granted: string[] = typeof token.scope === 'string' ? token.scope.split(/[\s,]+/) : [];
    return this.scopes().every(scope => granted.includes(scope));
  }

  async begin(): Promise<void> {
    if (this.running) return;
    if (!this.clientId || !this.callback || !this.china) { this.display('原包账号登录参数尚未就绪，请取消后从云存档入口重试。'); return; }
    this.running = true; const generation: number = ++this.generation;
    this.token = undefined; this.profile = undefined;
    try {
      this.display('正在连接 TapTap 官方账号授权…', '', true);
      const deviceEnvelope: DeviceEnvelope = JSON.parse(await this.network.request('LOGIN_DEVICE',
        'https://accounts.tapapis.cn/oauth2/v1/device/code', this.network.form(['client_id', this.clientId,
          'response_type', 'device_code', 'scope', this.scopes().join(',')]))) as DeviceEnvelope;
      if (generation !== this.generation) return;
      const device: LoginDevice | undefined = deviceEnvelope.data;
      if (deviceEnvelope.success !== true || !device?.device_code) throw new Error('无法获取账号授权码，请稍后重试');
      const link: string = device.verification_uri_complete ?? device.qrcode_url ?? device.verification_url ?? device.verification_uri ?? '';
      if (!/^https:\/\/([a-z0-9-]+\.)*(taptap\.cn|taptap\.com|taptap\.io|tapapis\.cn|tapapis\.com)(:443)?(\/|\?|$)/i.test(link)) throw new Error('账号授权地址未通过验证');
      this.display('请使用 TapTap 扫码，或打开官方授权页。' + (device.user_code ? '\n授权码：' + device.user_code : ''), link, true);
      const deadline: number = Date.now() + Math.min(Math.max(device.expires_in ?? 300, 30), 900) * 1000;
      let seconds: number = Math.min(Math.max(device.interval ?? 5, 3), 30);
      while (generation === this.generation && Date.now() < deadline) {
        await new Promise<void>((resolve) => setTimeout(resolve, seconds * 1000));
        if (generation !== this.generation) return;
        const result: TokenEnvelope = JSON.parse(await this.network.request('LOGIN_TOKEN', 'https://accounts.tapapis.cn/oauth2/v1/token',
          this.network.form(['grant_type', 'device_token', 'client_id', this.clientId, 'secret_type', 'hmac-sha-1', 'code', device.device_code]))) as TokenEnvelope;
        if (generation !== this.generation) return;
        const token: AccessToken | undefined = result.data; const error: string = token?.error ?? result.error ?? '';
        if (error === 'authorization_pending' || error === 'authorization_waiting') continue;
        if (error === 'slow_down') { seconds = Math.min(seconds + 5, 30); continue; }
        if (error) throw new Error('账号授权取消或已过期，请重试');
        if (result.success !== true || !token?.kid || !token.access_token || !token.mac_key) throw new Error('账号登录凭证不完整');
        if (!this.hasScopes(token)) throw new Error('TapTap 尚未授予原包请求的账号权限');
        this.token = token; break;
      }
      if (generation !== this.generation) return;
      if (!this.token) throw new Error('账号授权码已过期，请重试');
      this.display('账号已授权，正在读取 TapTap 资料…', '', true);
      const profile = await this.fetchProfile();
      if (generation !== this.generation) return;
      this.profile = profile; this.emit(0, JSON.stringify(this.token)); this.hide();
    } catch (error) {
      if (generation !== this.generation) return;
      this.token = undefined; this.profile = undefined;
      this.display(error instanceof Error ? error.message : '账号登录失败，请重试');
      hilog.error(0, 'TransSdk', 'LOGIN_FAILED (credentials and profile withheld)');
    } finally { if (generation === this.generation) this.running = false; }
  }

  private async fetchProfile(): Promise<TapProfile> {
    const token: AccessToken | undefined = this.token; if (!token) throw new Error('请先登录 TapTap');
    const granted: string[] = typeof token.scope === 'string' ? token.scope.split(/[\s,]+/) : [];
    const endpoint: string = 'https://open.tapapis.cn/account/' + (granted.includes('public_profile') ? 'profile' : 'basic-info') +
      '/v1?' + this.network.form(['client_id', this.clientId]);
    const generation: number = this.generation;
    const response: ProfileEnvelope = JSON.parse(await this.network.request('LOGIN_PROFILE', endpoint, '',
      await this.network.mac(endpoint, token))) as ProfileEnvelope;
    if (generation !== this.generation || response.success !== true || !response.data?.openid) throw new Error('无法读取有效账号资料');
    this.profile = response.data; return response.data;
  }

  cancel(): void {
    ++this.generation; this.running = false; this.token = undefined; this.profile = undefined;
    this.emit(1); this.hide();
  }
  stop(): void { ++this.generation; this.running = false; this.token = undefined; this.profile = undefined; }
}
