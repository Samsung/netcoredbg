/*
Send("1-file-exec-and-symbols dotnet");
Send("2-exec-arguments " + TestBin);
Send("3-exec-run");

var r = Expect("*stopped");
Assert.Equal("entry-point-hit", r.FindString("reason"));

Send(String.Format("4-break-insert -f {0}:{1}", TestSource, Lines["BREAK1"]));
Expect("4^done");

Send(String.Format("5-break-insert -f {0}:{1}", TestSource, Lines["BREAK2"]));
Expect("5^done");

Send("6-exec-continue");
*/

using System;

namespace SetValuesTest
{
    public struct TestStruct1
    {
        public int val1;
        public byte val2;

        public TestStruct1(int v1, byte v2)
        {
            val1 = v1;
            val2 = v2;
        }
    }

    public struct TestStruct2
    {
        public int val1;
        public TestStruct1 struct2;

        public TestStruct2(int v1, int v2, byte v3)
        {
            val1 = v1;
            struct2.val1 = v2;
            struct2.val2 = v3;
        }
    }

    class Program
    {
        static void Main(string[] args)
        {               // //@START@
            TestStruct2 ts = new TestStruct2(1, 5, 10); 

            bool testBool = false;
            char testChar = 'ㅎ';
            byte testByte = (byte)10;
            sbyte testSByte = (sbyte)-100;
            short testShort = (short)-500;
            ushort testUShort = (ushort)500;
            int testInt = -999999;
            uint testUInt = 999999;
            long testLong = -999999999;
            ulong testULong = 9999999999;

            decimal b = 0000001.000000000000000000000000006M;
            int[] arrs = decimal.GetBits(b);
            string testString = "someNewString that I'll test with";

            int dummy1 = 1;            // //@BREAK1@
/*
r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["BREAK1"], r.Find("frame").FindInt("line"));

Send(String.Format("8-var-create - * \"{0}\"", "ts.struct2.val1"));
r = Expect("8^done");
string val1 = r.FindString("name");
Send(String.Format("9-var-assign {0} \"666\"", val1));
r = Expect("9^done");

Send(String.Format("10-var-create - * \"{0}\"", "testBool"));
r = Expect("10^done");
string testBool = r.FindString("name");
Send(String.Format("11-var-assign {0} \"true\"", testBool));
r = Expect("11^done");

Send(String.Format("12-var-create - * \"{0}\"", "testChar"));
r = Expect("12^done");
string testChar = r.FindString("name");
Send(String.Format("13-var-assign {0} \"a\"", testChar));
r = Expect("13^done");

Send(String.Format("14-var-create - * \"{0}\"", "testByte"));
r = Expect("14^done");
string testByte = r.FindString("name");
Send(String.Format("15-var-assign {0} \"200\"", testByte));
r = Expect("15^done");

Send(String.Format("16-var-create - * \"{0}\"", "testSByte"));
r = Expect("16^done");
string testSByte = r.FindString("name");
Send(String.Format("17-var-assign {0} \"-1\"", testSByte));
r = Expect("17^done");

Send(String.Format("18-var-create - * \"{0}\"", "testShort"));
r = Expect("18^done");
string testShort = r.FindString("name");
Send(String.Format("19-var-assign {0} \"-666\"", testShort));
r = Expect("19^done");

Send(String.Format("20-var-create - * \"{0}\"", "testUShort"));
r = Expect("20^done");
string testUShort = r.FindString("name");
Send(String.Format("21-var-assign {0} \"666\"", testUShort));
r = Expect("21^done");

Send(String.Format("22-var-create - * \"{0}\"", "testInt"));
r = Expect("22^done");
string testInt = r.FindString("name");
Send(String.Format("23-var-assign {0} \"666666\"", testInt));
r = Expect("23^done");

Send(String.Format("24-var-create - * \"{0}\"", "testUInt"));
r = Expect("24^done");
string testUInt = r.FindString("name");
Send(String.Format("25-var-assign {0} \"666666\"", testUInt));
r = Expect("25^done");

Send(String.Format("26-var-create - * \"{0}\"", "testLong"));
r = Expect("26^done");
string testLong = r.FindString("name");
Send(String.Format("27-var-assign {0} \"-666666666\"", testLong));
r = Expect("27^done");

Send(String.Format("28-var-create - * \"{0}\"", "testULong"));
r = Expect("28^done");
string testULong = r.FindString("name");
Send(String.Format("29-var-assign {0} \"666666666\"", testULong));
r = Expect("29^done");

Send(String.Format("30-var-create - * \"{0}\"", "b"));
r = Expect("30^done");
string b = r.FindString("name");
Send(String.Format("31-var-assign {0} \"-1.000000000000000000000017\"", b));
r = Expect("31^done");

Send(String.Format("32-var-create - * \"{0}\"", "testString"));
r = Expect("32^done");
string testString = r.FindString("name");
Send(String.Format("33-var-assign {0} \"edited string\"", testString));
r = Expect("33^done");
Send("34-exec-continue");
*/
                 int dummy2 = 2;         // //@BREAK2@
/*
r = Expect("*stopped");
Assert.Equal("breakpoint-hit", r.FindString("reason"));
Assert.Equal(Lines["BREAK2"], r.Find("frame").FindInt("line"));

Send(String.Format("35-var-create - * \"{0}\"", "ts.struct2"));
r = Expect("35^done");
string struct2 = r.FindString("name");
Send(String.Format("36-var-list-children --simple-values \"{0}\"", struct2));
r = Expect("36^done");
string val = r.Find("children").Find("child").FindString("value");
Assert.Equal("666", val);






Send(String.Format("37-var-create - * \"{0}\"", "testBool"));
r = Expect("37^done");
Assert.Equal(r.FindString("value"), "true");

Send(String.Format("38-var-create - * \"{0}\"", "testChar"));
r = Expect("38^done");
Assert.Equal(r.FindString("value"), "97 \'a\'");

Send(String.Format("39-var-create - * \"{0}\"", "testByte"));
r = Expect("39^done");
Assert.Equal(r.FindString("value"), "200");

Send(String.Format("40-var-create - * \"{0}\"", "testSByte"));
r = Expect("40^done");
Assert.Equal(r.FindString("value"), "-1");

Send(String.Format("41-var-create - * \"{0}\"", "testShort"));
r = Expect("41^done");
Assert.Equal(r.FindString("value"), "-666");

Send(String.Format("42-var-create - * \"{0}\"", "testUShort"));
r = Expect("42^done");
Assert.Equal(r.FindString("value"), "666");

Send(String.Format("43-var-create - * \"{0}\"", "testInt"));
r = Expect("43^done");
Assert.Equal(r.FindString("value"), "666666");

Send(String.Format("44-var-create - * \"{0}\"", "testUInt"));
r = Expect("44^done");
Assert.Equal(r.FindString("value"), "666666");

Send(String.Format("45-var-create - * \"{0}\"", "testLong"));
r = Expect("45^done");
Assert.Equal(r.FindString("value"), "-666666666");

Send(String.Format("46-var-create - * \"{0}\"", "testULong"));
r = Expect("46^done");
Assert.Equal(r.FindString("value"), "666666666");

Send(String.Format("47-var-create - * \"{0}\"", "b"));
r = Expect("47^done");
Assert.Equal(r.FindString("value"), "-1.000000000000000000000017");

Send(String.Format("48-var-create - * \"{0}\"", "testString"));
r = Expect("48^done");
Assert.Equal(r.FindString("value"), "\"edited string\"");

*/
        }
    }
}
/*
Send("49-exec-continue");
r = Expect("*stopped");
Assert.Equal("exited", r.FindString("reason"));
*/
