# Mini DBMS with HTTP API Server

## 1. 프로젝트 배경 및 목표

기존에 만들었던 SQL 처리기와 DBMS를 외부 클라이언트에서 접근 가능하도록 HTTP API 서버로 확장하는 것이 목표입니다.

클라이언트는 HTTP 요청으로 SQL을 전송하고, 서버는 이를 파싱·실행한 뒤 결과를 JSON으로 반환합니다.  
지원 쿼리: `SELECT` (WHERE, BETWEEN 포함), `INSERT`

---

## 2. 아키텍처

```
Client (curl / wrk)
        │  HTTP (TCP)
        ▼
   TCP Socket Server
        │  accept()
        ▼
   Thread Pool ──────────────── Circular Queue
        │  worker thread
        ▼
   Dispatcher
        │  HTTP 파싱 → SQL 추출
        ▼
   Engine Adapter  ◀── pthread_rwlock (write lock)
        │
        ▼
   SQL Pipeline
   ┌────────────────────────────────┐
   │ Lexer → Parser → Schema 검증  │
   │          ↓                     │
   │       Executor                 │
   │          ↓                     │
   │    B+ Tree Index               │
   │          ↓                     │
   │      Data File (.dat)          │
   └────────────────────────────────┘
```

| 컴포넌트 | 역할 |
|---|---|
| TCP Socket Server | 클라이언트 연결 수락 (`accept` 루프) |
| Thread Pool | 고정 worker 스레드로 요청 처리 |
| Dispatcher | HTTP 파싱, SQL 추출, 응답 반환 |
| Engine Adapter | rwlock으로 SQL 실행 직렬화 |
| SQL Pipeline | Lexer → Parser → Schema → Executor |
| B+ Tree Index | 포인트·범위 쿼리 효율화 |

---

## 3. 핵심 구현

### 1) 네트워크 연결

TCP 소켓(`AF_INET`)을 열고 `SO_REUSEADDR`로 재시작 시 포트 충돌을 방지합니다.  
`accept` 루프에서 새 연결이 들어오면 Thread Pool에 작업을 제출하고 즉시 다음 연결을 대기합니다.  
Queue가 꽉 차면 `503 SERVICE_UNAVAILABLE`을 반환해 backpressure를 처리합니다.

```
클라이언트 연결
    → open_listen_socket()   # AF_INET, SO_REUSEADDR, backlog=1024
    → server_run()           # EINTR-safe accept loop
    → dispatcher_on_accept() # Thread Pool에 작업 제출
```

HTTP는 GET(쿼리 파라미터 `?sql=...`)과 POST(body) 모두 지원하며,  
Content-Length 기반으로 최대 65,536 bytes까지 읽습니다.

### 2) 동시성 제어

여러 클라이언트가 동시에 INSERT를 보내면 데이터 파일에 동시에 쓰려는 충돌이 발생합니다.  
이를 `pthread_rwlock`으로 해결합니다.

- **읽기(SELECT)**: 여러 스레드가 동시에 획득 가능 → 병렬 처리
- **쓰기(INSERT)**: 한 스레드만 독점 획득 → 다른 모든 읽기·쓰기 차단

```c
// engine_adapter.c
struct EngineAdapter {
    pthread_rwlock_t lock;
};

// 현재는 모든 쿼리에 write lock 적용 (Phase 1)
pthread_rwlock_wrlock(&adapter->lock);
execute_sql(...);
pthread_rwlock_unlock(&adapter->lock);
```

> B+ 트리의 fine-grained lock은 검토했으나, 구현 복잡도가 팀 학습 수준에 비해 과도하다고 판단해 global rwlock으로 대체했습니다.

### 3) Thread Pool

스레드를 요청마다 생성/소멸하면 context switching 비용이 커집니다.  
이를 방지하고자 고정 크기의 Thread Pool을 구현했습니다.

**구조**

```
Producer (accept loop)
    ↓  pthread_mutex + pthread_cond (not_full)
Circular Queue  [task0][task1]...[taskN]
    ↑  pthread_cond (not_empty)
Worker Threads  [W0][W1]...[Wk]
```

| 컴포넌트 | 역할 |
|---|---|
| `pthread_mutex_t mtx` | Queue 상태 보호 |
| `pthread_cond_t not_empty` | 작업 추가 시 worker 깨우기 |
| `pthread_cond_t not_full` | Queue 여유 공간 생길 때 producer 깨우기 |

**고정 스레드 선택 이유**  
동적 스레드 풀(부하에 따라 수 조절)도 고려했지만, 스레드 생성·소멸 오버헤드와 구현 복잡도 대비 성능 이득이 크지 않다고 판단했습니다.  
실무에서의 동적 운영 방식은 [쟁점 포인트](#5-쟁점-포인트)에서 다룹니다.

```
실행 예시
./sqlpd <port> [workers] [queue_capacity]
./sqlpd 8080 4 128
```

---

## 4. 데모

### 빌드 및 실행

```bash
# 빌드
make

# 테스트 데이터 생성 (200,000 rows)
make seed_users

# 서버 실행 (port=8080, workers=4, queue=128)
./sqlpd 8080 4 128
```

### SELECT

```bash
# 전체 조회
curl "http://localhost:8080/query?sql=SELECT%20*%20FROM%20users"

# 조건 조회
curl "http://localhost:8080/query?sql=SELECT%20*%20FROM%20users%20WHERE%20id%20%3D%201"

# 범위 조회
curl "http://localhost:8080/query?sql=SELECT%20*%20FROM%20users%20WHERE%20id%20BETWEEN%201%20AND%20100"
```

### INSERT

```bash
curl -X POST http://localhost:8080/query \
  -H "Content-Type: application/json" \
  -d '{"sql": "INSERT INTO users VALUES (999999, '\''alice'\'', 30, '\''alice@example.com'\'')"}'
```

### 응답 형식

**SELECT** — `text/plain` ASCII 테이블

```
+----+-------+-----+-------------------+
| id | name  | age | email             |
+----+-------+-----+-------------------+
| 1  | Alice | 30  | alice@example.com |
+----+-------+-----+-------------------+
(1 rows)
```

**INSERT** — `application/json`

```json
{"ok": true, "type": "insert", "affected_rows": 1}
```

**에러** — `application/json`

```json
{"ok": false, "error": {"code": "BAD_SQL", "message": "..."}}
```

---

## 5. 쟁점 포인트

### 1) 실무 DBMS Thread Pool 구성 방식

저희는 고정 크기 Thread Pool을 선택했지만, 실무 DBMS는 트래픽에 따라 스레드 수를 동적으로 운영합니다.

| DBMS | 방식 | 기본 스레드 수 | 특징 |
|---|---|---|---|
| MySQL | 고정 Thread Pool + 캐시 | 151개 | 스레드 재사용으로 생성 비용 절약 |
| PostgreSQL | 연결당 프로세스 | 100개 | 스레드 대신 프로세스, 안정성 높음 |
| MongoDB | 동적 Thread Pool | 부하에 따라 자동 조절 | 유연하지만 관리 복잡 |
| Java (ExecutorService) | 고정/동적 선택 가능 | 설정값 | mutex·queue·CV 추상화 제공 |

**공통 원칙**

- 기본값: **CPU 코어 수 × 2**
- 코어 수 초과 시 context switching 비용만 증가하고 처리량은 오르지 않음
- I/O 대기 구간에 다른 스레드가 실행될 수 있어 코어 수의 2배를 기준으로 사용

### 2) 시도해본 점 — 최적의 Thread 수와 Queue 길이 찾기

실제로는 더 많은 조건을 측정했지만, 핵심만 간단히 정리합니다.

Thread Pool의 `workers` 수와 Queue 길이가 서버 동작에 어떤 영향을 주는지 `wrk`로 비교했습니다.

**Workers 실험 (1 ~ 16개)**

INSERT 단독 부하와 SELECT·INSERT 혼합 부하를 측정했습니다.  
Mini DBMS에서는 SQL 실행 구간이 write lock으로 직렬화되어 있기 때문에,  
worker 수를 늘린다고 처리량이 선형으로 증가하지 않았습니다.

| 부하 유형 | 최적 workers | 결과 |
|---|---|---|
| INSERT 단독 | 2 | 가장 높은 median 처리량 |
| SELECT + INSERT 혼합 | 2 | socket error 없이 가장 안정적 |

**Queue 길이 실험**

`queue=500`은 속도에 있어 의미 있는 수치라고 보기는 어려웠습니다.  
Queue 실험의 의미는 수치보다 **backpressure 특성 확인**에 있었습니다.

- Queue가 작으면 → `503`·socket error 발생
- Queue가 충분히 크면 → 실패 대신 대기가 늘어남

**결론**

수치 자체보다는 Thread Pool 파라미터를 **처리량, 오류 여부, 부하 모델의 차이**까지 함께 해석했다는 점에 의미가 있습니다.

---

## 파일 구조

```
SQL Parser/
├── src/
│   ├── server_main.c        # 서버 진입점
│   ├── server/
│   │   ├── server.c         # TCP 소켓 서버
│   │   ├── threadpool.c     # Thread Pool
│   │   ├── dispatcher.c     # 요청 처리 및 라우팅
│   │   ├── engine_adapter.c # rwlock + SQL 실행 브릿지
│   │   ├── http_parser.c    # HTTP 요청 파싱
│   │   └── response.c       # HTTP 응답 포맷
│   ├── parser/parser.c      # SQL 파서
│   ├── input/lexer.c        # SQL 렉서
│   ├── executor/executor.c  # 쿼리 실행기
│   ├── schema/schema.c      # 스키마 로드·검증
│   └── bptree/bptree.c      # B+ 트리 인덱스
├── include/                 # 헤더 파일
├── schema/users.schema      # 테이블 스키마 정의
└── data/users.dat           # 데이터 파일
```
