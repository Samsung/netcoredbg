using System;
                namespace TestAppHotReload
                {
                    class Program
                    {
                        static void Main(string[] args)
                        {
                            Console.WriteLine("Hello World!");
                            HotReloadTest();
                        }
                        static void HotReloadTest()
                        {
                            Console.WriteLine("Initial string.");
                        }
                    }
                }
