"""Offline paired oracle for general function-body edits in large package / UVM contexts.

Use the common transport and measurements from body_queries. The method contains
external member / helper references, changing local types and declarations, loops,
nested blocks, and a deliberately unresolved symbol followed by recovery.
"""
import body_queries as benchmark

HEADER = '''class Accumulator;
  typedef struct { int value; string label; } payload_t;
  int marker;
  function int earlier(input int value); return value + marker; endfunction
  function int combine(input int alpha, input int bravo, input int charl);
    payload_t state;
    int local_0 = alpha;
    state.value = earlier(bravo) + tail_field;
    for (int i=0; i<charl; i++) begin
      local_0 += i;
    end
    return (local_0 + state.value);
  endfunction
  int tail_field;
  function int later(input int value); return value; endfunction
endclass
'''


def sequence():
    result = []
    begin = HEADER.index('    payload_t state;')
    end = HEADER.index('  endfunction', begin)
    for version in range(2, 14):
        kind = 'longint' if version % 2 else 'int'
        value = 'missing_user_symbol' if version in (5, 9) else 'earlier(bravo)'
        body = f'''    payload_t state;
    {kind} local_0 = alpha + {version};
    int values[] = new[2];
    state.value = {value} + tail_field;
    foreach (values[i]) begin
      string text = $sformatf("%0d", i);
      values[i] = i + state.value;
      if (text.len() > 0) local_0 += values[i];
    end
    return (local_0 + state.value);
'''
        result.append(HEADER[:begin] + body + HEADER[end:])
    return result


if __name__ == '__main__':
    benchmark.HEADER = HEADER
    benchmark.sequence = sequence
    benchmark.WORKLOAD_SCOPE = 'General body: changed declarations/types, member/helper calls, foreach, nested blocks, errors and recovery; synthetic large package with and without UVM.'
    benchmark.main()
