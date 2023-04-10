using System;using System.Diagnostics;using System.Threading;
                namespace TestAppHotReload
                {
                    class Program
                    {
                        static void Main(string[] args)
                        {   System.Threading.Thread.Sleep(500);
                            Console.WriteLine("Hello World!");
                            HotReloadTest();
                        }
                        static void HotReloadTest()
                        {
                            Console.WriteLine("Initial string.");
                        }
                    }
                }
