#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <time.h>
#include <memory>
#include "config.h"
#include "Manifest.h"

// ESP 的硬件串口接 STM32；Wi-Fi 客户端供 MQTT 复用。
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
// 同一串口混有 24 字节二进制遥测帧和以换行结束的文字命令。
static uint8_t frame[24];
static uint8_t used = 0;
static char uartLine[64];
static uint8_t uartLineUsed = 0;
static bool uartLineReady = false;
// MQTT 回调只记下升级请求，真正的网络下载留到 loop() 执行。
static bool otaRequested = false;
static bool otaBusy = false;
// 用上次尝试时间限制 Wi-Fi/MQTT 重连频率。
static unsigned long lastMqttTry = 0;
static unsigned long lastWifiTry = 0;
// 四个主题均由同一个设备 ID 拼出。
static char telemetryTopic[64];
static char ackTopic[64];
static char commandTopic[64];
static char statusTopic[64];

uint16_t crc16(const uint8_t *data, size_t count) {
  // Modbus CRC16：每字节依次处理 8 位，多项式的反射形式是 0xa001。
  uint16_t crc = 0xffff;
  for (size_t i = 0; i < count; ++i) {
    crc ^= data[i];
    for (int b = 0; b < 8; ++b)
      crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
  }
  return crc;
}
// 协议字段按小端存储：低字节在前。先读两个字节，再拼成四字节。
uint16_t readU16(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
uint32_t readU32(const uint8_t *p) { return (uint32_t)readU16(p) | ((uint32_t)readU16(p + 2) << 16); }

void onMqtt(char *topic, byte *payload, unsigned int length) {
  // 命令主题可收到 OTA 或 POLL:<毫秒>；payload 不保证以 '\0' 结尾。
  if (strcmp(topic, commandTopic) == 0) {
    if (length == 3 && memcmp(payload, "OTA", 3) == 0) {
      otaRequested = true;
      return;
    }
    if (length < 5 || length > 10 || memcmp(payload, "POLL:", 5) != 0) return;
    for (unsigned int i = 5; i < length; ++i)
      if (payload[i] < '0' || payload[i] > '9') return;
    // STM32 解析的是 SET:POLL:<毫秒>\n，这里补上 SET: 前缀。
    Serial.print("SET:");
    Serial.write(payload, length);
    Serial.print('\n');
    return;
  }
  // 收到 PC 对某个遥测序号的确认，再转发给 STM32 出队。
  if (strcmp(topic, ackTopic) != 0 || length == 0 || length > 10) return;
  uint32_t seq = 0;
  for (unsigned int i = 0; i < length; ++i) {
    if (payload[i] < '0' || payload[i] > '9' || seq > 429496729UL) return;
    uint32_t next = seq * 10UL + payload[i] - '0';
    if (next < seq) return;
    seq = next;
  }
  delay(10);  // 等 STM32 发完当前 24 字节帧，再把 ACK 发回去。
  Serial.print("ACK:"); Serial.println(seq);
}

void publishFrame(const uint8_t *p) {
  // 升级时停止转发；无 MQTT 连接时让 STM32 自行把帧留在 W25 队列。
  if (otaBusy || !mqtt.connected()) return;
  // V2 帧的 CRC 覆盖前 18 字节；V3 帧的 CRC 覆盖包含 CAN 值的前 20 字节。
  if (p[0] != 'G' || p[1] != 'W' || p[3] != 1) return;
  if (p[2] == 2) {
    if (crc16(p, 18) != readU16(p + 18)) return;
  } else if (p[2] == 3) {
    if (crc16(p, 20) != readU16(p + 20)) return;
  } else return;
  // 把二进制字段转成 MQTT JSON；seq 可用于 PC 去重和回 ACK。
  char body[250];
  int n = snprintf(body, sizeof body,
      "{\"device\":\"%s\",\"seq\":%lu,\"slave1_value\":%u,"
      "\"slave2_value\":%u,\"can_value\":%u,\"status\":%u,\"uptime_ms\":%lu}",
      DEVICE_ID, (unsigned long)readU32(p + 4),
      (unsigned)readU16(p + 8), (unsigned)readU16(p + 10),
      (unsigned)(p[2] == 3 ? readU16(p + 18) : 0),
      (unsigned)readU16(p + 12), (unsigned long)readU32(p + 14));
  if (n > 0 && n < (int)sizeof body) mqtt.publish(telemetryTopic, body);
}

void feedLine(uint8_t b) {
  // '\n' 结束一行，'\r' 忽略；不可打印字节会使当前半行作废。
  if (b == '\n') {
    uartLine[uartLineUsed] = 0;
    uartLineUsed = 0;
    uartLineReady = true;
  } else if (b != '\r' && b >= 32 && b <= 126 && uartLineUsed < sizeof uartLine - 1)
    uartLine[uartLineUsed++] = (char)b;
  else if (b != '\r') uartLineUsed = 0;
}

void feed(uint8_t b) {
  // 第一个 G 可能是二进制帧头；其他字符按文字行处理。
  if (used == 0) {
    if (b == 'G') frame[used++] = b;
    else feedLine(b);
    return;
  }
  if (used == 1 && b != 'W') {
    // 第二字节不为 W：把先前的 G 还给文字解析器，并重新处理当前字节。
    used = 0;
    feedLine('G');
    feed(b);
    return;
  }
  frame[used++] = b;
  // 帧长固定 24 字节，凑齐后校验并发布，再等待下一帧。
  if (used == sizeof frame) { publishFrame(frame); used = 0; }
}

static uint32_t blockCrc32(const uint8_t *p, size_t n) {
  // OTA 分块独立使用 CRC32；它只检查传输块，整包另用 SHA-256。
  uint32_t c = 0xffffffffUL;
  while (n--) {
    c ^= *p++;
    for (unsigned i=0;i<8;++i) c=(c>>1)^((c&1U)?0xedb88320UL:0U);
  }
  return c ^ 0xffffffffUL;
}
static int hexdigit(char c) {
  if(c>='0'&&c<='9')return c-'0';
  if(c>='a'&&c<='f')return c-'a'+10;
  if(c>='A'&&c<='F')return c-'A'+10;
  return -1;
}
static bool validSha(const String &s) {
  // SHA-256 的文本形式必须恰好是 64 个十六进制字符。
  if(s.length()!=64)return false;
  for(unsigned i=0;i<64;++i)if(hexdigit(s[i])<0)return false;
  return true;
}
static String field(const String &body,const char *key) {
  // manifest 每行形如 KEY=value，只匹配行首的键。
  String prefix=String(key)+"=";
  int at=body.indexOf(prefix);
  if(at<0||(at>0&&body[at-1]!='\n'))return String();
  int begin=at+prefix.length(),end=body.indexOf('\n',begin);
  if(end<0)end=body.length();
  String value=body.substring(begin,end);value.trim();return value;
}
static bool number(const String &s,uint32_t &out) {
  // 十进制转 uint32_t，遇到非数字或溢出就拒绝。
  if(s.isEmpty())return false;
  uint32_t value=0;
  for(unsigned i=0;i<s.length();++i) {
    if(s[i]<'0'||s[i]>'9')return false;
    uint32_t d=(uint32_t)(s[i]-'0');
    if(value>(0xffffffffUL-d)/10U)return false;
    value=value*10U+d;
  }
  out=value;return true;
}
static bool parseManifest(const String &body,Manifest &m) {
  // 校验产品标识、版本、应用区长度、摘要格式和下载 URL。
  m.product=field(body,"PRODUCT");m.sha=field(body,"SHA256");m.url=field(body,"URL");
  return m.product=="stm32f103c8-gateway"&&
         number(field(body,"VERSION"),m.version)&&
         number(field(body,"LENGTH"),m.length)&&
         m.version>0&&m.length>=8&&m.length<=40960&&
         validSha(m.sha)&&(m.url.startsWith("http://")||m.url.startsWith("https://"));
}
static bool makeClient(const String &url,WiFiClient *&client,
                       std::unique_ptr<BearSSL::X509List> &trust,
                       WiFiClient &plain,BearSSL::WiFiClientSecure &secure) {
  // client 是“指针的引用”，函数能替调用方选择 plain/secure；
  // unique_ptr 持有 CA 证书对象，避免证书指针提前失效。
  // HTTPS 必须有 CA 证书和可信时间，供 BearSSL 验证服务器证书。
  if(url.startsWith("https://")) {
    if(strlen(OTA_CA_CERT)<50 || time(nullptr)<1700000000L)return false;
    trust.reset(new BearSSL::X509List(OTA_CA_CERT));
    secure.setTrustAnchors(trust.get());secure.setX509Time(time(nullptr));
    client=&secure;return true;
  }
  // http:// 使用普通 TCP；它没有 TLS 身份验证。
  client=&plain;return url.startsWith("http://");
}
static bool getManifest(Manifest &m) {
  // 先读取小于等于 512 字节的发布清单，再按字段解析。
  WiFiClient plain;BearSSL::WiFiClientSecure secure;
  std::unique_ptr<BearSSL::X509List> trust;WiFiClient *client=nullptr;
  String url=OTA_MANIFEST_URL;
  if(!makeClient(url,client,trust,plain,secure))return false;
  HTTPClient http;http.setTimeout(12000);
  if(!http.begin(*client,url))return false;
  int code=http.GET();
  if(code!=HTTP_CODE_OK||http.getSize()>512) {http.end();return false;}
  String body=http.getString();http.end();
  return body.length()<=512&&parseManifest(body,m);
}
static bool readLine(String &line,uint32_t timeout) {
  // 等串口文字响应时，也持续把收到的字节喂给混合帧解析器。
  uint32_t start=millis();
  while(millis()-start<timeout) {
    if (uartLineReady) {
      line = uartLine; uartLineReady = false; return true;
    }
    if (Serial.available()) feed((uint8_t)Serial.read());
    delay(1);
  }
  return false;
}
static bool waitFor(const String &wanted,uint32_t timeout) {
  // 只把完全相等的整行作为目标回复，超时返回 false。
  String line;uint32_t start=millis();
  while(millis()-start<timeout) {
    if(readLine(line,200)) {
      if(line==wanted)return true;
      if(line=="FAIL"||line=="REJECT"||line=="BUSY"||line=="ROLLED_BACK"||line.endsWith("_ERR"))return false;
    }
  }
  return false;
}
static uint32_t deviceVersion() {
  // 升级前询问 STM32 当前确认过的版本；失败用 0 表示。
  while(Serial.available()) feed((uint8_t)Serial.read());
  uartLineReady = false;
  Serial.print("VERSION\n");
  String line;uint32_t start=millis();
  while(millis()-start<1800U) {
    if(readLine(line,200)&&line.startsWith("VERSION ")) {
      uint32_t value=0;
      if(number(line.substring(8),value))return value;
    }
  }
  return 0;
}
static bool fetchBytes(Stream &stream,uint8_t *out,size_t n,uint32_t timeout) {
  // 从 HTTP 流凑满 n 字节；每有数据到达就重置无进展超时。
  size_t got=0;uint32_t start=millis();
  while(got<n&&millis()-start<timeout) {
    int avail=stream.available();
    if(avail>0) {
      int count=(int)(n-got);if(count>avail)count=avail;
      int r=stream.readBytes((char *)out+got,count);
      if(r>0) {got+=(size_t)r;start=millis();}
    } else delay(1);
  }
  return got==n;
}
static bool sendBlock(uint16_t seq,const uint8_t *data,uint16_t n) {
  // 文本帧在换行处重同步；丢字节、坏 CRC 或 ACK 丢失都可以重发同一序号。
  char packet[300];
  int used=snprintf(packet,sizeof packet,"DATA %u %08lx ",(unsigned)seq,(unsigned long)blockCrc32(data,n));
  const char hex[]="0123456789abcdef";
  for(uint16_t i=0;i<n;++i) {packet[used++]=hex[data[i]>>4];packet[used++]=hex[data[i]&15U];}
  packet[used++]='\n';
  for(unsigned retry=0;retry<3;++retry) {
    Serial.write((const uint8_t *)packet,used);Serial.flush();
    uint32_t start=millis();String line;
    while(millis()-start<6000U) {
      if(!readLine(line,200))continue;
      if(line==String("ACK ")+seq)return true;
      if(line=="FAIL"||line=="REJECT")return false;
      if(line==String("NAK ")+seq)break;
    }
  }
  return false;
}
static bool performUpdate(const Manifest &m) {
  // 固件必须与 manifest 同源，避免清单指向另一个下载主机。
  String origin=OTA_MANIFEST_URL;
  int slash=origin.indexOf('/',origin.indexOf("://")+3);
  if(slash<0)return false;origin=origin.substring(0,slash);
  if(!m.url.startsWith(origin+"/"))return false;
  WiFiClient plain;BearSSL::WiFiClientSecure secure;
  std::unique_ptr<BearSSL::X509List> trust;WiFiClient *client=nullptr;
  if(!makeClient(m.url,client,trust,plain,secure))return false;
  HTTPClient http;http.setTimeout(20000);
  if(!http.begin(*client,m.url))return false;
  int code=http.GET();
  // HTTP 响应大小必须与清单的 LENGTH 精确一致。
  if(code!=HTTP_CODE_OK||http.getSize()!=(int)m.length) {http.end();return false;}
  WiFiClient *stream=http.getStreamPtr();
  while(Serial.available()) feed((uint8_t)Serial.read());
  uartLineReady = false;
  // 直接让正在运行的 App 接收固件，此时不请求复位。
  Serial.print("START ");Serial.print(m.version);Serial.print(' ');
  Serial.print(m.length);Serial.print(' ');Serial.print(m.sha);Serial.print('\n');
  // READY 表示 App 已逐扇区擦好候选区；期间采样任务继续运行。
  if(!waitFor("READY",120000)) {http.end();return false;}
  uint8_t block[128];uint32_t remain=m.length;uint16_t seq=0;
  while(remain) {
    // HTTP 内容按最多 128 字节拆包；发送成功才推进序号。
    uint16_t n=remain>sizeof block?sizeof block:(uint16_t)remain;
    if(!fetchBytes(*stream,block,n,5000)||!sendBlock(seq,block,n)) {
      Serial.print("ABORT\n");waitFor("ABORTED",2000);http.end();return false;
    }
    remain-=n;++seq;
  }
  // FINISH 要求 App 重读并校验整包，提交 STAGED 后才复位进入安装。
  Serial.print("FINISH\n");
  // 等待整包校验、旧版备份及安装；INSTALLED 仍不等于新版健康确认。
  bool ok=waitFor("INSTALLED",180000);http.end();return ok;
}
void setup() {
  // 初始化串口、设备专属 MQTT 主题、Wi-Fi、NTP 和 MQTT 回调。
  Serial.begin(115200, SERIAL_8N1);
  Serial.setTimeout(1000);
  snprintf(telemetryTopic, sizeof telemetryTopic, "gateway/%s/telemetry", DEVICE_ID);
  snprintf(ackTopic, sizeof ackTopic, "gateway/%s/ack", DEVICE_ID);
  snprintf(commandTopic, sizeof commandTopic, "gateway/%s/cmd", DEVICE_ID);
  snprintf(statusTopic, sizeof statusTopic, "gateway/%s/status", DEVICE_ID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setBufferSize(384);
  mqtt.setCallback(onMqtt);
}

void loop() {
  if (otaRequested) {
    // 回调设置的请求在主循环执行，避免在 MQTT 回调里阻塞下载。
    otaRequested = false;
    otaBusy = true;
    Manifest m;
    uint32_t current = deviceVersion();
    bool ok = current && getManifest(m) &&
              m.version > current && performUpdate(m);
    otaBusy = false;
    if (mqtt.connected()) mqtt.publish(statusTopic, ok ? "ota-transfer-complete" : "ota-failed", true);
    used = 0;
  }
  unsigned long now = millis();
  // millis() 是无符号毫秒计数，差值比较可跨常见的计数回绕。
  if (WiFi.status() != WL_CONNECTED && now - lastWifiTry > 10000UL) {
    lastWifiTry = now;
    WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  if (WiFi.status() == WL_CONNECTED && !mqtt.connected() && now - lastMqttTry > 5000UL) {
    lastMqttTry = now;
    // 连接时注册离线遗嘱；连上后订阅确认/控制并发布在线状态。
    if (mqtt.connect(DEVICE_ID, statusTopic, 0, true, "offline")) {
      mqtt.subscribe(ackTopic, 0); mqtt.subscribe(commandTopic, 0);
      mqtt.publish(statusTopic, "online", true);
    }
  }
  // mqtt.loop() 处理入站消息；串口轮询继续处理网关发出的数据。
  if (mqtt.connected()) mqtt.loop();
  while (Serial.available()) feed((uint8_t)Serial.read());
  delay(1);
}
