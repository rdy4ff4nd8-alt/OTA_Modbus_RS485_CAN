#pragma once
#include <Arduino.h>

// 一份发布清单解析后的字段：产品型号、整包摘要、下载 URL、版本和长度。
struct Manifest {
  String product, sha, url;
  uint32_t version = 0, length = 0;
};
