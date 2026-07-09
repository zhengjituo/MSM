# Module Calculator - 计算器示例模块

## 模块概述

**当前实现**：展示 MSM 框架的 IPC 通信模式

```
┌─────────────────────────────────────────────────────────────┐
│                   CalculatorMain (主进程)                    │
│  ┌──────────────┐  ┌────────────────┐  ┌─────────────────┐  │
│  │   AddModule  │  │ Message        │  │   IPCServer     │  │
│  │   (加法)     │  │ Bus            │  │  (UNIX Socket)  │  │
│  │  a, b, sum   │  │                │  │  /tmp/msm.sock  │  │
│  └──────────────┘  └────────────────┘  └─────────────────┘  │
│  ┌──────────────┐                                           │
│  │ MultiplyModule│                                          │
│  │   (乘法)     │                                           │
│  │  a, b, result│                                          │
│  └──────────────┘                                           │
└─────────────────────────────────────────────────────────────┘
          │                              │
          └──────────────┬───────────────┘
                       │
               外部客户端 (Python/socat)
```

## 构建运行

```bash
cd /mnt/hgfs/share/opencode/MSM/module_calculator/build
cmake --build .
./calculator_main &
```

## IPC 通信协议 (UNIX Socket)

**Socket 路径**: `/tmp/msm.sock`

**连接方式**:
```bash
socat - /tmp/msm.sock
```

**命令格式** (REST-style):
| 命令 | 格式 | 说明 |
|------|------|------|
| GET | `GET Module:var` | 获取变量值 |
| PUT | `PUT Module:var value` | 设置变量值 |
| LIST | `LIST [Module]` | 列出模块变量 |
| SCHEMA | `SCHEMA [Module]` | 显示模块结构 |

### 使用示例

```bash
# 设置 AddModule 的 a=10, b=20
echo "PUT AddModule:a 10" | socat - /tmp/msm.sock
echo "PUT AddModule:b 20" | socat - /tmp/msm.sock

# 触发计算 (action=1 执行 sum=a+b)
echo "PUT AddModule:action 1" | socat - /tmp/msm.sock

# 获取结果
echo "GET AddModule:sum" | socat - /tmp/msm.sock
# 返回: {"OK":30}

# 列出所有变量
echo "LIST AddModule" | socat - /tmp/msm.sock
```

### Python 客户端

```python
import socket

s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect('/tmp/msm.sock')

s.send(b'PUT AddModule:a 10\n')
print(s.recv(1024).decode().strip())  # {"OK":10}

s.send(b'PUT AddModule:b 20\n')
print(s.recv(1024).decode().strip())  # {"OK":20}

s.send(b'PUT AddModule:action 1\n')
print(s.recv(1024).decode().strip())  # {"OK":0}

s.send(b'GET AddModule:sum\n')
print(s.recv(1024).decode().strip())  # {"OK":30}

s.close()
```

## 模块变量

### AddModule
| 变量 | 类型 | 说明 |
|------|------|------|
| `a` | int | 加数 |
| `b` | int | 被加数 |
| `sum` | int | 结果 (a+b) |
| `action` | int | 触发器 (设1执行计算) |

### MultiplyModule
| 变量 | 类型 | 说明 |
|------|------|------|
| `a` | int | 因数 |
| `b` | int | 因数 |
| `result` | int | 结果 (a×b) |
| `action` | int | 触发器 (设1执行计算) |

## 验证结果

```
10 + 20 = 30  ✓
7 × 8 = 56    ✓
```

## 当前限制

- ⚠️ **计算模式**: 通过 `action=1` 触发，非消息驱动
- ⚠️ **变量类型**: 仅支持 int 类型
- ⚠️ **IPC 解析**: 命令必须用空格分隔 (不支持 `PUT AddModule:a=10`)

## Task 历史

- **20260509_01**: Module Calculator 示例 ✅
- **20260514_01**: IPC 通信验证 + 文档完善 ✅