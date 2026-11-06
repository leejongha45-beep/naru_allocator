# naru_allocator

독립 C++23 정적 라이브러리 패키지. Conan 참조는 `naru_allocator/0.1.0`, CMake 타깃은 `naru::allocator`다.

## 아키텍처

```mermaid
flowchart TB
    LIB["naru_allocator<br/>C++23 정적 라이브러리"]

    subgraph GROUP["GroupAllocator"]
        GAPI["공개 API<br/>galloc · greserve · gfree · gclean"]
        GIMPL["GroupAllocator::Impl<br/>크기 변환 · 할당 흐름"]
        STORAGE["MemorySegListStorage"]

        GAPI -->|"StaticPimpl 내부 저장"| GIMPL
        GIMPL --> STORAGE

        STATE["CoreGroupHeap × 256<br/>VM 시작 주소 · currentOffset<br/>예약/커밋 페이지 정보"]
        LOCK["std::mutex × 256<br/>그룹별 잠금"]
        FREE["Free MemorySegListVector × 256<br/>크기별 Free 리스트"]
        USED["Used MemorySegListVector × 256<br/>크기별 Used 리스트"]

        STORAGE --> STATE
        STORAGE --> LOCK
        STORAGE --> FREE
        STORAGE --> USED

        FLIST["MemorySegList<br/>Free 구간의 이중 연결 리스트<br/>삭제한 노드 슬롯 재사용"]
        ULIST["MemorySegList<br/>Used 구간의 이중 연결 리스트<br/>삭제한 노드 슬롯 재사용"]
        SORTED["SortedFreeSegVector<br/>Free 주소와 크기 수집<br/>주소순 정렬 · 인접 구간 병합"]

        FREE --> FLIST
        USED --> ULIST
        FREE --> SORTED

        GHEAP["그룹별 데이터 VM 영역"]
        STATE --> GHEAP
        FLIST -.->|"구간 시작 주소 참조"| GHEAP
        ULIST -.->|"구간 시작 주소 참조"| GHEAP
    end

    subgraph OWNED["OwnedAllocator"]
        OAPI["공개 API<br/>createVal · oalloc · ofree"]
        OIMPL["OwnedAllocator::Impl<br/>원소별 할당 단위 · 할당 커서<br/>예약/커밋 크기"]
        OHEAP["인스턴스별 데이터 VM 영역<br/>Bump-pointer 순차 할당"]

        OAPI -->|"StaticPimpl 내부 저장"| OIMPL
        OIMPL --> OHEAP
    end

    LIB --> GAPI
    LIB --> OAPI

    PLATFORM["공통 VM 인터페이스<br/>platform/memory.hpp<br/>페이지 크기 조회 · 예약 · 커밋 · 해제"]

    STORAGE -.->|"데이터·메타데이터 VM 요청"| PLATFORM
    OIMPL -.->|"데이터 VM 요청"| PLATFORM

    PLATFORM --> LINUX["Linux 백엔드<br/>sysconf<br/>mmap · mprotect · munmap"]
    PLATFORM --> WINDOWS["Windows 백엔드<br/>GetSystemInfo<br/>VirtualAlloc · VirtualFree"]

    SUPPORT["naru_support<br/>StaticPimpl<br/>로그 · 계약 검사<br/>결과 반환형 · 재시도"]
    LIB -.->|"의존 패키지"| SUPPORT
```

Free·Used 리스트의 노드는 메타데이터용 VM에 저장되고, 각 노드가 가진 시작 주소가 그룹 데이터 VM의 구간을 가리킵니다.

## 상세 설명

**1. 가상 메모리 계층**

데이터와 메타데이터 저장공간을 OS 가상 메모리로 관리합니다. 페이지 크기를 조회해 예약·커밋 범위를 계산하고, 플랫폼별 시스템 호출을 공통 인터페이스로 감쌌습니다.

```mermaid
flowchart LR
    REQ["데이터·메타데이터<br/>메모리 요청"] --> VM["platform/memory.hpp"]

    VM --> L["Linux"]
    VM --> W["Windows"]

    L --> LP["sysconf<br/>페이지 크기 조회"]
    L --> LR["mmap · mprotect · munmap<br/>예약 · 접근 권한 변경 · 해제"]

    W --> WP["GetSystemInfo<br/>페이지 크기 조회"]
    W --> WR["VirtualAlloc · VirtualFree<br/>예약 · 커밋 · 해제"]
```

**2. GroupAllocator의 메타데이터 구조**

그룹 ID로 `std::array`를 인덱싱합니다. 각 그룹의 데이터 힙과 크기별 Free·Used 목록을 분리하고, 목록의 노드는 메타데이터용 VM에 배치합니다.

```mermaid
flowchart TB
    ID["groupId<br/>0 ~ 255"] --> STORAGE["MemorySegListStorage<br/>std::array"]
    STORAGE --> HEAP["CoreGroupHeap<br/>VM 주소 · currentOffset · 페이지 상태"]
    STORAGE --> FREE["Free MemorySegListVector"]
    STORAGE --> USED["Used MemorySegListVector"]
    STORAGE --> MUTEX["그룹별 std::mutex"]

    FREE --> FL["크기별 MemorySegList"]
    USED --> UL["크기별 MemorySegList"]

    FL --> FN["이중 연결 리스트 노드"]
    UL --> UN["이중 연결 리스트 노드"]

    FN -.->|"Segment.pBaseAddr"| DATA["그룹 데이터 VM"]
    UN -.->|"Segment.pBaseAddr"| DATA
    HEAP --> DATA
```

각 리스트는 제거된 노드 슬롯을 재사용 스택에 보관합니다. 새 노드가 필요하면 이 슬롯부터 사용합니다.

```mermaid
flowchart LR
    LIVE["사용 중인 메타데이터 노드"] -->|"eraseAt()"| STACK["해당 리스트의<br/>재사용 노드 스택"]
    STACK -->|"다음 emplaceBack()"| LIVE
    VM["노드용 VM의 다음 슬롯"] -->|"재사용 슬롯이 없을 때"| LIVE
```

**3. 구간 할당과 재사용**

요청 크기를 16B 단위로 올린 `S`를 사용합니다. 실제 주소를 `std::uintptr_t`로 변환해 `% S`로 정렬을 계산합니다. 앞패딩은 Free 구간으로 회수하며, 분할 크기는 `std::bit_width()`와 `std::countr_zero()`로 선택합니다.

```mermaid
flowchart TD
    ALLOC["galloc"] --> FIND{"S 크기 Free 구간"}
    FIND -->|"있음"| POP["Free에서 구간 하나 꺼내기"]
    FIND -->|"없음"| LARGE{"크기가 S의 배수인<br/>큰 Free 구간"}
    LARGE -->|"있음"| SPLIT["S 단위로 분할"]
    LARGE -->|"없음"| ACQUIRE["실제 주소 정렬<br/>앞패딩 회수 · 범위 확보"]
    SPLIT --> READY["S 크기 Free 리스트에 등록"]
    ACQUIRE --> READY
    READY -->|"galloc에서 사용"| POP
    POP --> USED["S 크기 Used 리스트에 등록"]
    USED --> PTR["시작 주소 반환"]

    RESERVE["greserve"] --> BATCH["정렬된 S × 개수 범위 확보"]
    BATCH --> READY
```

반환은 같은 크기의 목록 사이에서 주소를 이동합니다. 병합은 주소순으로 정렬한 Free 구간 중 맞닿은 범위만 대상으로 합니다.

```mermaid
flowchart LR
    USED["Used"] -->|"gfree"| FREE["Free"]
    FREE -->|"gclean"| SORT["std::ranges::sort<br/>주소순 정렬"]
    SORT --> MERGE["인접 범위 병합"]
    MERGE --> ALIGN["실제 주소에 맞춰<br/>다시 분할"]
    ALIGN --> FREE
```

**4. OwnedAllocator의 순차 할당**

원소 크기를 16B 단위로 올려 고정 할당 단위를 정합니다. Bump-pointer 방식으로 요청량만큼 커서를 이동하고, `ofree()`에서 커서를 시작점으로 되돌립니다.

```mermaid
flowchart LR
    CREATE["createVal(elementSize)"] --> UNIT["고정 할당 단위 설정"]
    UNIT --> VM["VM 예약 · 초기 커밋"]
    VM --> ALLOC["oalloc(count)"]
    ALLOC --> SPACE["필요한 페이지 추가 커밋"]
    SPACE --> CURSOR["할당 단위 × count만큼<br/>커서 전진"]
    CURSOR --> RETURN["시작 주소 반환"]
    RESET["ofree"] --> ZERO["커서 = 0"]
    ZERO --> ALLOC
```

**5. 인터페이스와 실행 제어**

`StaticPimpl`로 구현 타입을 `.cpp`에 두고, 구현 객체의 저장공간은 공개 객체 내부에 둡니다. `GroupAllocator`는 `consteval` 입력 타입으로 타입 크기와 지원 정렬을 확인합니다.

```mermaid
flowchart LR
    TYPE["std::type_identity&lt;T&gt;"] --> INFO["consteval TypeInfo<br/>sizeof · alignof 검사"]
    INFO --> API["공개 API"]
    API --> PIMPL["StaticPimpl<br/>alignas + std::byte 저장공간"]
    PIMPL --> IMPL["Impl<br/>구현은 .cpp에 정의"]

    IMPL --> LOCK["GroupAllocator<br/>std::scoped_lock"]
    IMPL --> CHECK["NARU_CHECK<br/>Debug 계약 검사"]
    IMPL --> RESULT["std::expected 기반<br/>내부 결과 반환"]
    RESULT --> RETRY["NARU_TRY<br/>필요한 목록 생성·커밋 후 재시도"]
```

**6. 빌드와 검증**

CMake로 정적 라이브러리와 설치 대상을 구성하고, Conan으로 의존성을 선언합니다. GoogleTest·CTest와 설치 소비자 예제로 내부 동작과 공개 인터페이스를 검증합니다.

```mermaid
flowchart LR
    DEPS["Conan 의존성<br/>naru_support · GoogleTest"] --> CMAKE["CMake"]
    CMAKE --> LIB["naru::allocator<br/>정적 라이브러리"]
    CMAKE --> TEST["GoogleTest · CTest<br/>할당 · 반환 · 정렬 · 병합"]
    LIB --> INSTALL["공개 헤더와<br/>라이브러리 설치"]
    INSTALL --> CONSUMER["설치 소비자 예제<br/>GroupAllocator · OwnedAllocator"]
```
