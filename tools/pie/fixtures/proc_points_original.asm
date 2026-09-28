ee.vld.128.ip q0, %[sx], 0
ee.vld.128.ip q1, %[sy], 0
ee.vldbc.16.ip q3, %[p], 2
ee.vldbc.16.ip q2, %[p], 2
ee.mov.s16.qacc q2
ee.vldbc.16.ip q2, %[p], 2
ee.vmulas.s16.qacc q2, q3
ee.vldbc.16.ip q2, %[p], 2
ee.vmulas.s16.qacc q2, q3
ee.vldbc.16.ip q2, %[p], 2
ee.vmulas.s16.qacc q2, q3
ee.vldbc.16.ip q2, %[p], 2
ee.vmulas.s16.qacc q0, q2
ee.vldbc.16.ip q2, %[p], 2
ee.vmulas.s16.qacc q1, q2
ee.srcmb.s16.qacc q4, %[shift], 0
ee.vst.128.ip q4, %[dx], 0
ee.vldbc.16.ip q2, %[p], 2
ee.mov.s16.qacc q2
ee.vldbc.16.ip q2, %[p], 2
ee.vmulas.s16.qacc q2, q3
ee.vldbc.16.ip q2, %[p], 2
ee.vmulas.s16.qacc q2, q3
ee.vldbc.16.ip q2, %[p], 2
ee.vmulas.s16.qacc q2, q3
ee.vldbc.16.ip q2, %[p], 2
ee.vmulas.s16.qacc q0, q2
ee.vldbc.16.ip q2, %[p], 2
ee.vmulas.s16.qacc q1, q2
ee.srcmb.s16.qacc q4, %[shift], 0
ee.vst.128.ip q4, %[dy], 0
