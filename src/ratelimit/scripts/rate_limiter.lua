-- Atomic Distributed Token Bucket Rate Limiter
-- Evaluates token bucket state using authoritative Redis server time.
--
-- KEYS[1]: Rate limit state key (e.g. 'ratelimit:groq:minute')
-- ARGV[1]: Tokens requested (number, e.g. 1)
-- ARGV[2]: Bucket capacity (number, e.g. 30)
-- ARGV[3]: Refill rate per second (number, e.g. 0.5)
-- ARGV[4]: Key expiration TTL in seconds (number, e.g. 60)
--
-- Returns array: { allowed (0 or 1), remaining_tokens (integer), retry_after_ms (integer) }

local key = KEYS[1]
local requested = tonumber(ARGV[1])
local capacity = tonumber(ARGV[2])
local fill_rate = tonumber(ARGV[3])
local ttl = tonumber(ARGV[4])

-- Get server-authoritative time
local time = redis.call('TIME')
local now_ms = (tonumber(time[1]) * 1000) + math.floor(tonumber(time[2]) / 1000)

-- Fetch current bucket state
local data = redis.call('HMGET', key, 'tokens', 'last_refill_ms')
local current_tokens = capacity
local last_refill_ms = now_ms

if data[1] and data[2] then
    current_tokens = tonumber(data[1])
    last_refill_ms = tonumber(data[2])
    local elapsed_sec = math.max(0, (now_ms - last_refill_ms) / 1000.0)
    current_tokens = math.min(capacity, current_tokens + (elapsed_sec * fill_rate))
end

if current_tokens >= requested then
    current_tokens = current_tokens - requested
    redis.call('HMSET', key, 'tokens', tostring(current_tokens), 'last_refill_ms', tostring(now_ms))
    redis.call('EXPIRE', key, ttl)
    return { 1, math.floor(current_tokens), 0 }
else
    local needed = requested - current_tokens
    local retry_after_ms = math.ceil((needed / fill_rate) * 1000)
    redis.call('HMSET', key, 'tokens', tostring(current_tokens), 'last_refill_ms', tostring(now_ms))
    redis.call('EXPIRE', key, ttl)
    return { 0, math.floor(current_tokens), retry_after_ms }
end

